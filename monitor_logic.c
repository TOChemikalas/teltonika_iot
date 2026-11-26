#include "monitor_logic.h"

#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

// Verify that a device ID is safe to use
bool valid_device_id(const char *id)
{
    if (id == NULL)
        return false;
    size_t length = strlen(id);
    if (length == 0 || length >= MAX_DEVICE_ID) // max device is 64
        return false;
    // Validate every character
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

static bool json_digit(char ch) // check if a character is a digit for JSON number validation
{
    return ch >= '0' && ch <= '9';
}

// Check number spelling before cJSON's permissive numeric conversion.
static bool valid_json_numbers(const char *json)
{
    const char *p = json;
    while (*p != '\0') {
        // Skip JSON strings
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                }
                p++;
            }
            if (*p == '\0')
                return false;
            p++;
         //Identify numbers   
        } else if (*p == '-' || json_digit(*p)) {
            if (*p == '-')
                p++;
        // Check integer portion
            if (*p == '0') {
                p++;
            } else if (*p >= '1' && *p <= '9') {
                do { p++; } while (json_digit(*p));
            } else {
                return false;
            }
            // Validate decimal numbers
            if (*p == '.') {
                p++;
                if (!json_digit(*p))
                    return false;
                do { p++; } while (json_digit(*p));
            }
            // Validate scientific notation
            if (*p == 'e' || *p == 'E') {
                p++;
                if (*p == '+' || *p == '-')
                    p++;
                if (!json_digit(*p))
                    return false;
                do { p++; } while (json_digit(*p));
            }
            // What follows the number
            if (*p != '\0' && strchr(" \t\r\n,]}", *p) == NULL)
                return false;
        } else {
            p++;
        }
    }
    return true;
}

 // Detect actual JSON Unicode NUL escapes, ignoring escaped backslashes.
static bool contains_json_nul_escape(const char *json)
{
    bool in_string = false;

    for (const char *p = json; *p != '\0'; p++) {
        if (*p == '"') {
            in_string = !in_string;
        // Detect a backslash
        } else if (in_string && *p == '\\') {
            if (strncmp(p + 1, "u0000", 5) == 0)
                return true;

            if (p[1] == '\0')
                break;

            p++; // Skip the escaped character.
        }
    }

    return false;
}

bool parse_telemetry(const void *payload, size_t length, struct Telemetry *output, const char **error)
{
    // Check incoming payload
    const char *reason = "Invalid MQTT payload size or content.";
    cJSON *root = NULL;
    char json[MAX_JSON]; // MAX_JSON is 4096
    if (payload == NULL || length == 0 || length >= sizeof(json) ||
        memchr(payload, '\0', length) != NULL)
        goto invalid;
    
    memcpy(json, payload, length);
    json[length] = '\0';
    // cJSON strings cannot represent an embedded NUL without truncation.
    if (contains_json_nul_escape(json))
        goto invalid;

    reason = "Invalid JSON number syntax.";
    if (!valid_json_numbers(json))
        goto invalid;

    reason = "Invalid JSON object.";
    root = cJSON_ParseWithOpts(json, NULL, 1);
    if (!cJSON_IsObject(root))
        goto invalid;

    const char *names[] = {"device_id", "temperature", "humidity", "battery", "timestamp"};
    
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int count = 0;
        const cJSON *field;
        cJSON_ArrayForEach(field, root) {
            if (field->string != NULL && strcmp(field->string, names[i]) == 0)
                count++;
        }
        reason = "Missing or duplicate sensor field.";
        if (count != 1)
            goto invalid;
    }
    // Verify data types and device ID
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON *hum = cJSON_GetObjectItemCaseSensitive(root, "humidity");
    const cJSON *bat = cJSON_GetObjectItemCaseSensitive(root, "battery");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    reason = "Incorrectly typed sensor field.";
    if (!cJSON_IsString(id) || !cJSON_IsNumber(temp) || !cJSON_IsNumber(hum) ||
        !cJSON_IsNumber(bat) || !cJSON_IsNumber(ts))
        goto invalid;
    reason = "Invalid device ID.";
    if (!valid_device_id(id->valuestring))
        goto invalid;

    double battery = bat->valuedouble;
    double timestamp = ts->valuedouble;
    reason = "Sensor value out of range or not an integer.";
    // Validate numerical ranges
    if (!isfinite(temp->valuedouble) || !isfinite(hum->valuedouble) ||
        hum->valuedouble < 0 || hum->valuedouble > 100 ||
        !isfinite(battery) || battery < 0 || battery > 100 ||
        battery != (double)(int)battery ||
        !isfinite(timestamp) || timestamp < 0 || timestamp > 9007199254740991.0 ||
        timestamp != (double)(long long)timestamp)
        goto invalid;

    // Copy the validated values
    struct Telemetry sensor = {0};
    strcpy(sensor.device_id, id->valuestring);
    sensor.temperature = temp->valuedouble;
    sensor.humidity = hum->valuedouble;
    sensor.battery = (int)battery;
    sensor.timestamp = (long long)timestamp;
    *output = sensor;
    cJSON_Delete(root);
    if (error != NULL)
        *error = NULL;
    return true;

    // Cleanup and error reporting
invalid:
    cJSON_Delete(root);
    if (error != NULL)
        *error = reason;
    return false;
}

bool update_device(struct DeviceStore *store, const struct Telemetry *sensor)
{
    // Search for existing device
    size_t index = 0;
    while (index < store->count &&
           strcmp(store->items[index].device_id, sensor->device_id) != 0)
        index++;
    // Allocate more memmory if needed
    if (index == store->count) {
        if (store->count == store->capacity) {
            if (store->capacity > SIZE_MAX / 2)
                return false;
            size_t capacity = store->capacity == 0 ? 8 : store->capacity * 2;
            if (capacity > SIZE_MAX / sizeof(*store->items))
                return false;
            // Reallocate the items array to the new capacity
            struct Telemetry *items = realloc(store->items, capacity * sizeof(*items));
            if (items == NULL)
                return false;
            store->items = items;
            store->capacity = capacity;
        }
        store->count++;
    }
    store->items[index] = *sensor;
    return true;
}

// Release device memory
void free_devices(struct DeviceStore *store)
{
    free(store->items);
    *store = (struct DeviceStore){0};
}

// Construct a MQTT topic
bool make_alert_topic(const char *device_id, char *output, size_t size)
{
    if (!valid_device_id(device_id) || output == NULL || size == 0)
        return false;
    int length = snprintf(output, size, "iot/devices/%s/alert", device_id);
    return length >= 0 && (size_t)length < size;
}

// Calculate warnings
struct Warnings evaluate_thresholds(const struct Telemetry *sensor, const struct Thresholds *thresholds)
{
    return (struct Warnings) {
        sensor->temperature > thresholds->temperature,
        sensor->humidity > thresholds->humidity,
        sensor->battery < thresholds->battery
    };
}

// Validate decimal thresholds
bool parse_number(const char *text, double minimum, double maximum, double *output)
{
    if (text == NULL || *text == '\0' || isspace((unsigned char)*text))
        return false;
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < minimum || value > maximum)
        return false;
    *output = value;
    return true;
}

// Validate unsigned integers
bool parse_uint(const char *text, unsigned int maximum, unsigned int *output)
{
    if (text == NULL || *text == '\0')
        return false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > maximum)
        return false;
    *output = (unsigned int)value;
    return true;
}

uint64_t elapsed_seconds(struct timespec received, struct timespec now)
{
    if (now.tv_sec < received.tv_sec ||
        (now.tv_sec == received.tv_sec && now.tv_nsec < received.tv_nsec))
        return 0;

    uint64_t seconds = (uint64_t)now.tv_sec - (uint64_t)received.tv_sec;
    if (now.tv_nsec < received.tv_nsec)
        seconds--;
    return seconds;
}

bool device_inactive(const struct Telemetry *sensor, struct timespec now,
                     unsigned int inactive_after)
{
    return elapsed_seconds(sensor->received_at, now) >= inactive_after;
}

void format_elapsed(uint64_t seconds, char output[32])
{
    if (seconds < 60)
        snprintf(output, 32, "%" PRIu64 "s", seconds);
    else
        snprintf(output, 32, "%" PRIu64 "m %" PRIu64 "s", seconds / 60, seconds % 60);
}
