#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <float.h>
#include <getopt.h>
#include <pthread.h>
#include "monitor_logic.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>
#include <cjson/cJSON.h>

#if ATOMIC_BOOL_LOCK_FREE != 2 // check if the atomic_bool is lock-free
#error "This program requires lock-free atomic_bool for signal handling"
#endif

static struct DeviceStore devices = {0};
static bool connected = false;
static bool interactive = false;
static struct timespec last_status_check = {0};
static unsigned int inactive_after = 10;
static struct Thresholds thresholds = {30.0, 80.0, 20.0};
static int mqtt_qos = 0;
static size_t pending_alerts = 0;
static size_t acknowledged_alerts = 0;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file = NULL;
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static atomic_bool subscription_failed = ATOMIC_VAR_INIT(false);
static atomic_bool runtime_failed = ATOMIC_VAR_INIT(false);

static void handle_stop(int signal_number) // pressing Ctrl+C, we tell the system to stop the program, 
{                                          // but we don't exit immediately, we let the main loop finish its work and exit gracefully
    (void)signal_number;
    atomic_store_explicit(&stop_requested, true, memory_order_relaxed);
}

static void format_timestamp(long long timestamp, char output[32]) // make the timestamp human readable - format: YYYY-MM-DD HH:MM:SS
{
    time_t seconds = (time_t)timestamp;
    struct tm *date = localtime(&seconds);

    if (date == NULL || strftime(output, 32, "%Y-%m-%d %H:%M:%S", date) == 0) {
        snprintf(output, 32, "%lld", timestamp);
    }
}


 // Caller must hold state_mutex while the MQTT loop is running.
static void display_devices(bool force)
{
    // Get the current time
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("Cannot read monotonic clock");
        atomic_store(&runtime_failed, true);
        atomic_store(&stop_requested, true);
        return;
    }

    size_t inactive_count = 0;
    bool status_changed = false;

    // Detect devices that became inactive or active since the last check.
    for (size_t i = 0; i < devices.count; i++) {
        const struct Telemetry *sensor = &devices.items[i];

        bool inactive = device_inactive(sensor, now, inactive_after);
        bool previously_inactive =
            device_inactive(sensor, last_status_check, inactive_after);

        if (inactive)
            inactive_count++;

        if (inactive != previously_inactive)
            status_changed = true;
    }

    last_status_check = now;

    // Avoid redundant output when stdout is not an interactive terminal.
    if (!interactive && !force && !status_changed)
        return;

    if (interactive)
        printf("\033[H\033[J");

    printf("IoT Device Monitor | Broker: %s | Devices: %zu total | Active: %zu | Inactive: %zu\n\n",
           connected ? "connected" : "disconnected",
           devices.count, devices.count - inactive_count, inactive_count);

    if (mqtt_qos == 1)
        printf("Alert QoS 1 | Pending: %zu | Acknowledged: %zu\n",
               pending_alerts, acknowledged_alerts);

    printf("Active devices\n\n");
    printf("     Device      %15s %12s %12s %-19s  %s\n",
           "Temperature(C)", "Humidity(%)", "Battery(%)",
           "Last update", "Elapsed / Warnings");
    printf("----------------------------------------------------------------------------------------------------\n");

    for (size_t i = 0; i < devices.count; i++) {
        const struct Telemetry *sensor = &devices.items[i];

        if (device_inactive(sensor, now, inactive_after))
            continue;

        char age[32];
        char date[32];

        format_elapsed(elapsed_seconds(sensor->received_at, now), age);
        format_timestamp(sensor->timestamp, date);

        struct Warnings warnings = evaluate_thresholds(sensor, &thresholds);

        // Center the device ID in its 16-character column.
        int id_length = (int)strlen(sensor->device_id);
        int padding = id_length < 16 ? (16 - id_length) / 2 : 0;

        printf("%*s%-*s %9.2f %13.2f %9d         %-19s  ",
               padding, "", 16 - padding, sensor->device_id,
               sensor->temperature, sensor->humidity,
               sensor->battery, date);

        printf("%-10s ", age);

        if (!warnings.high_temperature && !warnings.high_humidity &&!warnings.low_battery) {
            printf("OK");
        } else {
            if (warnings.high_temperature)
                printf("HIGH_TEMPERATURE ");
            if (warnings.high_humidity)
                printf("HIGH_HUMIDITY ");
            if (warnings.low_battery)
                printf("LOW_BATTERY");
        }

        printf("\n");
    }

    if (inactive_count == devices.count)
        printf("  None\n");

    printf("\nInactive devices%s\n\n",
           connected || inactive_count == 0
               ? "" : " - STALE (broker offline)");

    if (inactive_count == 0)
        printf("  None\n");

    for (size_t i = 0; i < devices.count; i++) {
        const struct Telemetry *sensor = &devices.items[i];

        if (!device_inactive(sensor, now, inactive_after))
            continue;

        char age[32];
        format_elapsed(elapsed_seconds(sensor->received_at, now), age);

        printf("  %-16s %s\n", sensor->device_id, age);
    }

    if (interactive)
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

    pthread_mutex_lock(&state_mutex);
    connected = true;
    int rc = mosquitto_subscribe(mosq, NULL, "iot/devices/+/telemetry", mqtt_qos); // upon connection, recconection we subscribe to the MQTT topic
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Subscribe error: %s\n", mosquitto_strerror(rc));
        atomic_store(&subscription_failed, true);
    }
    display_devices(true);
    pthread_mutex_unlock(&state_mutex);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int result)
{

    (void)mosq;
    (void)userdata;
    pthread_mutex_lock(&state_mutex);
    connected = false;

    // Report unexpected disconnections; the MQTT loop handles reconnection.
    if (result != 0 &&
        !atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        fprintf(stderr, "Broker connection lost; trying to reconnect.\n");
        display_devices(true);
    }

    pthread_mutex_unlock(&state_mutex);
}

static void on_subscribe(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos) // check if the broker accepted our subscription request
{
    (void)mosq;
    (void)userdata;
    (void)mid;
    if (qos_count < 1 || granted_qos == NULL || granted_qos[0] >= 0x80) {
        fprintf(stderr, "Broker rejected telemetry subscription.\n");
        atomic_store(&subscription_failed, true);
    } else {
        fprintf(stderr, "Telemetry subscription granted QoS %d (requested %d).\n",
                granted_qos[0], mqtt_qos);
    }
}

static void on_publish(struct mosquitto *mosq, void *userdata, int mid) // Track how many MQTT QoS 1 alerts have been acknowledged by the broker.
{
    (void)mosq;
    (void)userdata;
    (void)mid;
    if (mqtt_qos != 1)
        return;

    pthread_mutex_lock(&state_mutex);

    // Count only acknowledgments for alerts recorded as pending.
    if (pending_alerts > 0) {
        pending_alerts--;
        acknowledged_alerts++;
    }

    pthread_mutex_unlock(&state_mutex);
}

// Caller holds state_mutex.
// Build and queue an MQTT alert, then log successful publish requests.
static void publish_alert(struct mosquitto *mosq, const struct Telemetry *sensor, const char *type, double value) 
{
    char topic[128];

    if (!make_alert_topic(sensor->device_id, topic, sizeof(topic))) {
        fprintf(stderr, "Invalid MQTT alert topic.\n");
        return;
    }

    cJSON *alert = cJSON_CreateObject();
    if (alert == NULL) {
        fprintf(stderr, "Cannot allocate alert JSON.\n");
        return;
    }
    // Add alert info
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
        int mid;
        int rc = mosquitto_publish(mosq, &mid, topic, (int)strlen(payload), payload, mqtt_qos, false);
        // QoS 1 acknowledgments are handled later by on_publish().
        if (rc == MOSQ_ERR_SUCCESS && mqtt_qos == 1)
            pending_alerts++;
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

// Record rejected MQTT messages in the optional log file
static void log_rejected_telemetry(const char *topic, const char *reason)
{
    pthread_mutex_lock(&state_mutex);
    if (log_file != NULL) {
        //Prepare a safe topic string
        char safe_topic[256];
        size_t used = 0;
        const unsigned char *p = (const unsigned char *)(topic != NULL ? topic : "(unknown)");
        for (; *p != '\0'; p++) {
            // Escape non-printable characters and truncate if necessary.
            bool escape = *p < 0x20 || *p >= 0x7f || *p == '\\' || *p == '|';
            size_t needed = escape ? 4 : 1;
            // Reserve space for a truncation marker and keep each record on one line.
            if (used + needed > sizeof(safe_topic) - 4) {
                memcpy(safe_topic + used, "...", 3);
                used += 3;
                break;
            }
            // Write escaped characters
            if (escape) {
                snprintf(safe_topic + used, 5, "\\x%02X", (unsigned int)*p);
                used += 4;
            } else {
                safe_topic[used++] = (char)*p;
            }
        }
        safe_topic[used] = '\0';
        //write rejection record to log file
        int written = fprintf(log_file, "%lld | REJECTED | Topic: %s | Reason: %s\n",
                              (long long)time(NULL), safe_topic, reason);
        if (written < 0 || fflush(log_file) == EOF) {
            perror("Rejected telemetry log write failed");
            fclose(log_file);
            log_file = NULL;
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

static void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    (void)userdata;
    if (atomic_load_explicit(&stop_requested, memory_order_relaxed))
        return;

    struct timespec received_at;
    if (clock_gettime(CLOCK_MONOTONIC, &received_at) != 0) {
        perror("Cannot read telemetry receipt time");
        atomic_store(&runtime_failed, true);
        atomic_store(&stop_requested, true);
        return;
    }

    struct Telemetry sensor;
    // Validate incoming telemetry
    const char *error;

    if (message->payloadlen <= 0 || !parse_telemetry(message->payload, (size_t)message->payloadlen, &sensor, &error)) {
        const char *reason = message->payloadlen <= 0 ? "Empty telemetry payload." : error;
        fprintf(stderr, "%s\n", reason);
        log_rejected_telemetry(message->topic, reason);
        return;
    }
    // Update the device
    sensor.received_at = received_at;

    pthread_mutex_lock(&state_mutex);
    if (!update_device(&devices, &sensor)) {
        pthread_mutex_unlock(&state_mutex);
        fprintf(stderr, "Device allocation failed or capacity overflow.\n");
        return;
    }

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

    struct Warnings warnings = evaluate_thresholds(&sensor, &thresholds);
    if (warnings.high_temperature)
        publish_alert(mosq, &sensor, "HIGH_TEMPERATURE", sensor.temperature);
    if (warnings.high_humidity)
        publish_alert(mosq, &sensor, "HIGH_HUMIDITY", sensor.humidity);
    if (warnings.low_battery)
        publish_alert(mosq, &sensor, "LOW_BATTERY", sensor.battery);

    display_devices(true);
    pthread_mutex_unlock(&state_mutex);
}

// Display available command-line options, their accepted values and defaults.
static void print_usage(const char *program)
{
    printf("Usage: %s [-H HOST] [-P PORT] [-o FILE] [OPTIONS]\n", program);
    printf("  -H HOST   MQTT broker address (default: localhost)\n");
    printf("  -P PORT   MQTT broker port, 1..65535 (default: 1883)\n");
    printf("  -o FILE   Append telemetry, alerts and rejected-message records to a file\n");
    printf("  --inactive-after SECONDS  Integer 1..%d (default: 10)\n", INT_MAX);
    printf("  --temp VALUE      Alert above this finite temperature (default: 30 C)\n");
    printf("  --humidity VALUE  Alert above this percentage, 0..100 (default: 80)\n");
    printf("  --battery VALUE   Alert below this percentage, 0..100 (default: 20)\n");
    printf("  --qos 0|1         Subscription and alert QoS (default: 0)\n");
    printf("  -h        Show this help message\n");
}

int main(int argc, char *argv[])
{
    // Default settings
    const char *host = "localhost";
    const char *log_path = NULL;
    int port = 1883;

    enum { OPT_INACTIVE = 256, OPT_TEMP, OPT_HUMIDITY, OPT_BATTERY, OPT_QOS };
    static const struct option options[] = {
        {"inactive-after", required_argument, NULL, OPT_INACTIVE},
        {"temp", required_argument, NULL, OPT_TEMP},
        {"humidity", required_argument, NULL, OPT_HUMIDITY},
        {"battery", required_argument, NULL, OPT_BATTERY},
        {"qos", required_argument, NULL, OPT_QOS},
        {NULL, 0, NULL, 0}
    };

    int option;
    // Parse and validate command-line options before initializing the monitor.
    while ((option = getopt_long(argc, argv, "+H:P:o:h", options, NULL)) != -1) {
        unsigned int number;
        bool valid = true;
        switch (option) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'H':
            host = optarg;
            valid = host[0] != '\0';
            break;
        case 'P':
            valid = parse_uint(optarg, 65535, &number) && number > 0;
            if (valid)
                port = (int)number;
            break;
        case 'o':
            log_path = optarg;
            valid = log_path[0] != '\0';
            break;
        case OPT_INACTIVE:
            valid = parse_uint(optarg, INT_MAX, &number) && number > 0;
            if (valid)
                inactive_after = number;
            break;
        case OPT_QOS:
            valid = parse_uint(optarg, 1, &number);
            if (valid)
                mqtt_qos = (int)number;
            break;
        case OPT_TEMP:
            valid = parse_number(optarg, -DBL_MAX, DBL_MAX, &thresholds.temperature);
            break;
        case OPT_HUMIDITY:
            valid = parse_number(optarg, 0, 100, &thresholds.humidity);
            break;
        case OPT_BATTERY:
            valid = parse_number(optarg, 0, 100, &thresholds.battery);
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
        if (!valid) {
            fprintf(stderr, "Invalid value: %s (see -h for allowed ranges).\n", optarg);
            return 1;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return 1;
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

    const char *term = getenv("TERM");
    interactive = isatty(STDOUT_FILENO) && term != NULL && term[0] != '\0' && strcmp(term, "dumb") != 0;
    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("Cannot install shutdown handlers");
        goto cleanup;
    }

    // Initialize MQTT
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
    mosquitto_publish_callback_set(mosq, on_publish);

    // Start MQTT connection
    int rc = mosquitto_connect_async(mosq, host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT connection setup failed: %s\n",
                mosquitto_strerror(rc));
        goto cleanup;
    }

    printf("Starting IoT monitor for %s:%d.\n", host, port);
    display_devices(true);
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Cannot start MQTT loop: %s\n", mosquitto_strerror(rc));
        goto cleanup;
    }
    loop_started = true;
    
    // Application loop
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed) && !atomic_load(&subscription_failed)) {
        sleep(1);
        pthread_mutex_lock(&state_mutex);
        display_devices(false);
        pthread_mutex_unlock(&state_mutex);
    }

    exit_code = atomic_load(&subscription_failed) || atomic_load(&runtime_failed) ? 1 : 0;

// stop MQTT loop and disconnect from broker, free allocated memory, close log file, cleanup MQTT library
cleanup:
    atomic_store(&stop_requested, true);
    if (mosq != NULL && loop_started && mqtt_qos == 1) {
        struct timespec start, now;
        bool clock_ready = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
        for (;;) {
            pthread_mutex_lock(&state_mutex);
            bool waiting = pending_alerts > 0 && connected;
            pthread_mutex_unlock(&state_mutex);
            if (!waiting || !clock_ready || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
                elapsed_seconds(start, now) >= 3)
                break;
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
        }
    }
    if (mosq != NULL) {
        mosquitto_disconnect(mosq);

        if (loop_started)
            mosquitto_loop_stop(mosq, false);

        mosquitto_destroy(mosq);
    }

    if (mqtt_qos == 1 && loop_started) {
        fprintf(stderr, "QoS 1 alerts: %zu acknowledged, %zu pending at shutdown.\n",
                acknowledged_alerts, pending_alerts);
        if (pending_alerts > 0)
            fprintf(stderr, "Unacknowledged alerts may be lost when this process exits.\n");
    }
    free_devices(&devices);

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
