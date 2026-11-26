#ifndef MONITOR_LOGIC_H
#define MONITOR_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MAX_DEVICE_ID 64
#define MAX_JSON 4096

struct Telemetry {
    char device_id[MAX_DEVICE_ID];
    double temperature;
    double humidity;
    int battery;
    long long timestamp;
    struct timespec received_at;
};

struct Thresholds {
    double temperature;
    double humidity;
    double battery;
};

struct Warnings {
    bool high_temperature;
    bool high_humidity;
    bool low_battery;
};

struct DeviceStore {
    struct Telemetry *items;
    size_t count;
    size_t capacity;
};

bool valid_device_id(const char *id);
bool parse_telemetry(const void *payload, size_t length, struct Telemetry *output,
                     const char **error);
bool update_device(struct DeviceStore *store, const struct Telemetry *sensor);
void free_devices(struct DeviceStore *store);
bool make_alert_topic(const char *device_id, char *output, size_t size);

struct Warnings evaluate_thresholds(const struct Telemetry *sensor,
                                    const struct Thresholds *thresholds);
bool parse_number(const char *text, double minimum, double maximum, double *output);
bool parse_uint(const char *text, unsigned int maximum, unsigned int *output);

uint64_t elapsed_seconds(struct timespec received, struct timespec now);
bool device_inactive(const struct Telemetry *sensor, struct timespec now,
                     unsigned int inactive_after);
void format_elapsed(uint64_t seconds, char output[32]);

#endif
