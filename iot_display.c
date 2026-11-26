#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>
#include <cjson/cJSON.h>

#define MAX_DEVICE_ID 64
#define MAX_JSON 4096
#define HIGH_TEMP_THRESHOLD 30.0
#define HIGH_HUM_THRESHOLD 80.0
#define LOW_BATTERY_THRESHOLD 20

#if ATOMIC_BOOL_LOCK_FREE != 2 //patikrinam ar signalui naudojamas atomic bool veikia be uzraktu
#error "This program requires lock-free atomic_bool for SIGINT handling"
#endif

struct Telemetry {
    char device_id[MAX_DEVICE_ID];
    double temperature;
    double humidity;
    int battery;
    long long timestamp;
};

static struct Telemetry *devices = NULL;
static size_t device_count = 0;
static size_t device_capacity = 0;
static bool connected = false;
static FILE *log_file = NULL;
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static atomic_bool subscription_failed = ATOMIC_VAR_INIT(false);

static void handle_sigint(int signal_number) // paspaudus Ctrl+C, pranesam programai kad laikas sustoti
{
    (void)signal_number;
    atomic_store_explicit(&stop_requested, true, memory_order_relaxed);
}

static void format_timestamp(long long timestamp, char output[32]) // padaryt timestamp zmogui skaitomu formatu YYYY-MM-DD HH:MM:SS
{
    time_t seconds = (time_t)timestamp;
    struct tm *date = localtime(&seconds);

    if (date == NULL || strftime(output, 32, "%Y-%m-%d %H:%M:%S", date) == 0) { 
        snprintf(output, 32, "%lld", timestamp);
    }
}



static void display_devices(void) // atnaujinam terminala su naujausiais irenginiu duomenim
{
    printf("\033[H\033[J");

    printf("IoT Device Monitor | Broker: %s | Devices: %zu\n\n",
           connected ? "connected" : "disconnected", device_count);

    printf("     Device     %15s %12s %12s %-19s  %s\n",
           "Temperature(C)", "Humidity(%)", "Battery(%)",
           "Last update", "Warnings");

    printf("----------------------------------------------------------------------------------------------------\n");

    for (size_t i = 0; i < device_count; i++) {
        const struct Telemetry *sensor = &devices[i];

        char date[32];
        format_timestamp(sensor->timestamp, date);

        bool high_temp = sensor->temperature > HIGH_TEMP_THRESHOLD;
        bool high_hum = sensor->humidity > HIGH_HUM_THRESHOLD;
        bool low_bat = sensor->battery < LOW_BATTERY_THRESHOLD;

        // +-centruojam irenginio ID jo stulpelyje
        int id_length = (int)strlen(sensor->device_id);
        int padding = id_length < 16 ? (16 - id_length) / 2 : 0;

        printf("%*s%-*s %9.2f %13.2f %9d         %-19s  ",
            padding, "",
            16 - padding, sensor->device_id,
            sensor->temperature,
            sensor->humidity,
            sensor->battery,
            date);

        if (!high_temp && !high_hum && !low_bat) {
            printf("OK");
        } else {
            if (high_temp)
                printf("HIGH_TEMPERATURE ");
            if (high_hum)
                printf("HIGH_HUMIDITY ");
            if (low_bat)
                printf("LOW_BATTERY");
        }

        printf("\n");
    }

    printf("\nPress Ctrl+C to exit.\n");
    fflush(stdout);
}




static void on_connect(struct mosquitto *mosq, void *userdata, int result)
{
    (void)userdata;
    if (result != 0) {
        fprintf(stderr, "Broker refused connection (code %d)\n", result);
        return;
    }

    connected = true;
    int rc = mosquitto_subscribe(mosq, NULL, "iot/devices/+/telemetry", 0); // prisijungus uzsiprenumeruojam MQTT tema
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Subscribe error: %s\n", mosquitto_strerror(rc));
        atomic_store(&subscription_failed, true);
    }
    display_devices();
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int result)
{
    (void)mosq;
    (void)userdata;
    connected = false;
    if (result != 0 && !atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        fprintf(stderr, "Broker connection lost; trying to reconnect.\n");
        display_devices();
    }
}

static void on_subscribe(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos) // patikrinimas ar brokeris leido subscribint
{
    (void)mosq;
    (void)userdata;
    (void)mid;
    if (qos_count < 1 || granted_qos == NULL || granted_qos[0] >= 0x80) {
        fprintf(stderr, "Broker rejected telemetry subscription.\n");
        atomic_store(&subscription_failed, true);
    }
}

static void publish_alert(struct mosquitto *mosq, const struct Telemetry *sensor, const char *type, double value) // sukuriamas JSON ispejimas ir issiunciams per MQTT
{
    char topic[128];

    int length = snprintf(topic, sizeof(topic), "iot/devices/%s/alert", sensor->device_id);

    if (length < 0 || (size_t)length >= sizeof(topic)) {
        fprintf(stderr, "MQTT alert topic too long.\n");
        return;
    }

    cJSON *alert = cJSON_CreateObject();
    if (alert == NULL) {
        fprintf(stderr, "Cannot allocate alert JSON.\n");
        return;
    }

    if (cJSON_AddStringToObject(alert, "device_id", sensor->device_id) == NULL ||
        cJSON_AddStringToObject(alert, "alert", type) == NULL ||
        cJSON_AddNumberToObject(alert, "value", value) == NULL ||
        cJSON_AddNumberToObject(alert, "timestamp", (double)sensor->timestamp) == NULL) {

        fprintf(stderr, "Cannot build alert JSON.\n");
        cJSON_Delete(alert);
        return;
    }

    char *payload = cJSON_PrintUnformatted(alert);
    if (payload != NULL) {
        int rc = mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, 0, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Alert publish error: %s\n", mosquitto_strerror(rc));
        
        } else if (log_file != NULL) {
            int written = fprintf(log_file, "%lld | %s | ALERT: %s | Value: %.2f\n", sensor->timestamp, sensor->device_id, type, value);
            if (written < 0 || fflush(log_file) == EOF) {
                perror("Alert log write failed");
                fclose(log_file);
                log_file = NULL;
            }
        }

        cJSON_free(payload);
    } else {
        fprintf(stderr, "Cannot serialize alert JSON.\n");
    }
    cJSON_Delete(alert);
}

static void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    (void)userdata;
    char json[MAX_JSON];

    // patikrinimas ar gautas pranesimas nera tuscias arba per didelis
    if (message->payload == NULL || message->payloadlen <= 0 ||
        message->payloadlen >= (int)sizeof(json) ||
        memchr(message->payload, '\0', (size_t)message->payloadlen) != NULL) {

        fprintf(stderr, "Invalid MQTT payload size or content.\n");
        return;
    }

    memcpy(json, message->payload, (size_t)message->payloadlen);
    json[message->payloadlen] = '\0';

    cJSON *root = cJSON_ParseWithOpts(json, NULL, 1);
    if (!cJSON_IsObject(root)) {
        fprintf(stderr, "Invalid JSON object.\n");
        cJSON_Delete(root);
        return;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON *hum = cJSON_GetObjectItemCaseSensitive(root, "humidity");
    const cJSON *bat = cJSON_GetObjectItemCaseSensitive(root, "battery");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "timestamp");

    // patikrinimas ar iJSON turi visus reikalingus duomenis
    if (!cJSON_IsString(id) || id->valuestring == NULL ||
        !cJSON_IsNumber(temp) || !cJSON_IsNumber(hum) ||
        !cJSON_IsNumber(bat) || !cJSON_IsNumber(ts)) {

        fprintf(stderr, "Missing or incorrectly typed sensor field.\n");
        cJSON_Delete(root);
        return;
    }

    struct Telemetry sensor;
    size_t id_length = strlen(id->valuestring);
    if (id_length == 0 || id_length >= sizeof(sensor.device_id)) {
        fprintf(stderr, "Invalid device ID length.\n");
        cJSON_Delete(root);
        return;
    }

    // patikrinimas ar irenginio ID neturi netinkamu simboliu
    for (size_t i = 0; i < id_length; i++) {
        unsigned char ch = (unsigned char)id->valuestring[i];
        if (!isalnum(ch) && ch != '_' && ch != '-') {
            fprintf(stderr, "Invalid device ID character.\n");
            cJSON_Delete(root);
            return;
        }
    }

    double battery = bat->valuedouble;
    double timestamp = ts->valuedouble;
    if (!isfinite(temp->valuedouble) || !isfinite(hum->valuedouble) ||
        hum->valuedouble < 0 || hum->valuedouble > 100 ||
        !isfinite(battery) || battery < 0 || battery > 100 ||
        battery != (double)(int)battery ||
        !isfinite(timestamp) || timestamp < 0 ||
        timestamp > 9007199254740991.0 ||
        timestamp != (double)(long long)timestamp) {

        fprintf(stderr, "Sensor value out of range or not an integer.\n");
        cJSON_Delete(root);
        return;
    }

    strcpy(sensor.device_id, id->valuestring);
    sensor.temperature = temp->valuedouble;
    sensor.humidity = hum->valuedouble;
    sensor.battery = (int)battery;
    sensor.timestamp = (long long)timestamp;

    
    size_t index = 0;

    while (index < device_count && strcmp(devices[index].device_id, sensor.device_id) != 0) {
        index++;
    }
    // jei naujam irenginiui neuztenka vietos, dinamiskai padidinamas masyvas
    if (index == device_count) {
        if (device_count == device_capacity) {
            size_t new_capacity = device_capacity == 0 ? 8 : device_capacity * 2;
            if (new_capacity < device_capacity || new_capacity > SIZE_MAX / sizeof(*devices)) {
                fprintf(stderr, "Device capacity overflow.\n");
                cJSON_Delete(root);
                return;
            }

            struct Telemetry *new_devices =
                realloc(devices, new_capacity * sizeof(*devices));

            if (new_devices == NULL) {
                fprintf(stderr, "Device memory allocation failed.\n");
                cJSON_Delete(root);
                return;
            }

            devices = new_devices;
            device_capacity = new_capacity;
        }

        device_count++;
    }

    devices[index] = sensor;
        
    if (log_file != NULL) {
        int written = fprintf(log_file,
                            "%lld | %s | Temp: %.2f C | Hum: %.2f %% | Battery: %d %%\n",
                            sensor.timestamp, sensor.device_id,
                            sensor.temperature, sensor.humidity, sensor.battery);

        if (written < 0 || fflush(log_file) == EOF) {
            perror("Telemetry log write failed");
            fclose(log_file);
            log_file = NULL;
        }
    }

    if (sensor.temperature > HIGH_TEMP_THRESHOLD)
        publish_alert(mosq, &sensor, "HIGH_TEMPERATURE", sensor.temperature);
    if (sensor.humidity > HIGH_HUM_THRESHOLD)
        publish_alert(mosq, &sensor, "HIGH_HUMIDITY", sensor.humidity);
    if (sensor.battery < LOW_BATTERY_THRESHOLD)
        publish_alert(mosq, &sensor, "LOW_BATTERY", sensor.battery);

    display_devices();
    cJSON_Delete(root);
}

static void print_usage(const char *program)
{
    printf("Usage: %s [-H HOST] [-P PORT] [-o FILE] [-h]\n", program);
    printf("  -H HOST   MQTT broker address (default: localhost)\n");
    printf("  -P PORT   MQTT broker port (default: 1883)\n");
    printf("  -o FILE   Append received telemetry and alerts to a file\n");
    printf("  -h        Show this help message\n");
}

int main(int argc, char *argv[])
{
    const char *host = "localhost";
    const char *log_path = NULL;
    int port = 1883;

    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "-H") != 0 && strcmp(argv[i], "-P") != 0 && strcmp(argv[i], "-o") != 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return 1;
        }

        if (strcmp(argv[i], "-H") == 0) {
            host = argv[++i];

            if (host[0] == '\0') {
                fprintf(stderr, "Host cannot be empty.\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-P") == 0) {
            char *end;
            errno = 0;
            long number = strtol(argv[++i], &end, 10);

            if (errno != 0 || *end != '\0' || number < 1 || number > 65535) {
                fprintf(stderr, "Port must be between 1 and 65535.\n");
                return 1;
            }

            port = (int)number;
        } else if (strcmp(argv[i], "-o") == 0) {
            log_path = argv[++i];
        }
    }


    if (log_path != NULL) {
        log_file = fopen(log_path, "a");
        if (log_file == NULL) {
            perror("Cannot open log file");
            return 1;
        }
    }

    int exit_code = 1;
    bool library_ready = false;
    bool loop_started = false;
    struct mosquitto *mosq = NULL;

    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        perror("Cannot install Ctrl+C handler");
        goto cleanup;
    }

    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT library initialization failed.\n");
        goto cleanup;
    }
    library_ready = true;

    mosq = mosquitto_new(NULL, true, NULL);
    if (mosq == NULL) {
        fprintf(stderr, "Cannot create MQTT client.\n");
        goto cleanup;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_subscribe_callback_set(mosq, on_subscribe);
    mosquitto_message_callback_set(mosq, on_message);

    int rc = mosquitto_connect_async(mosq, host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT connection setup failed: %s\n",
                mosquitto_strerror(rc));
        goto cleanup;
    }

    system("clear");
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Cannot start MQTT loop: %s\n", mosquitto_strerror(rc));
        goto cleanup;
    }
    loop_started = true;

    printf("Starting IoT monitor for %s:%d. Press Ctrl+C to stop.\n", host, port);
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed) && !atomic_load(&subscription_failed))
        sleep(1);

    exit_code = atomic_load(&subscription_failed) ? 1 : 0;

// sustabdom MQTT ir atlaisvinama naudota atmintis
cleanup:
    if (mosq != NULL) {
        mosquitto_disconnect(mosq);

        if (loop_started)
            mosquitto_loop_stop(mosq, false);

        mosquitto_destroy(mosq);
    }

    free(devices);

    if (library_ready)
        mosquitto_lib_cleanup();


    if (log_file != NULL) {
        if (fclose(log_file) == EOF) {
            perror("Cannot close log file");
        }
        log_file = NULL;
    }

    return exit_code;

}
