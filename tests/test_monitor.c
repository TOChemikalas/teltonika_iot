#include "monitor_logic.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

static unsigned int checks;

#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static const char valid_json[] =
    "{\"device_id\":\"sensor_01\",\"temperature\":31.5,\"humidity\":81.5,"
    "\"battery\":19,\"timestamp\":1795648002}";

static bool parse(const char *json, struct Telemetry *sensor)
{
    return parse_telemetry(json, strlen(json), sensor, NULL);
}

static void test_valid_telemetry(void)
{
    struct Telemetry sensor;
    const char *error = "unchanged";
    CHECK(parse_telemetry(valid_json, strlen(valid_json), &sensor, &error));
    CHECK(error == NULL);
    CHECK(strcmp(sensor.device_id, "sensor_01") == 0);
    CHECK(sensor.temperature == 31.5 && sensor.humidity == 81.5);
    CHECK(sensor.battery == 19 && sensor.timestamp == 1795648002LL);
    CHECK(sensor.received_at.tv_sec == 0 && sensor.received_at.tv_nsec == 0);
    CHECK(parse(" {\"device_id\":\"x-Y_1\",\"temperature\":-1e2,\"humidity\":0,"
                "\"battery\":100,\"timestamp\":0,\"extra\":true} \n", &sensor));
    CHECK(sensor.temperature == -100 && sensor.battery == 100);
}

static void test_missing_and_malformed(void)
{
    const char *fields[] = {"device_id", "temperature", "humidity", "battery", "timestamp"};
    struct Telemetry sensor = {.battery = 42};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        cJSON *root = cJSON_Parse(valid_json);
        CHECK(root != NULL);
        cJSON_DeleteItemFromObjectCaseSensitive(root, fields[i]);
        char *json = cJSON_PrintUnformatted(root);
        CHECK(json != NULL);
        CHECK(!parse(json, &sensor));
        CHECK(sensor.battery == 42);
        cJSON_free(json);
        cJSON_Delete(root);

        const char *bad_values[] = {"null", "true", "{}", "[]", "\"12\""};
        for (size_t j = 0; j < sizeof(bad_values) / sizeof(bad_values[0]); j++) {
            if (i == 0 && j == 4)
                continue;
            root = cJSON_Parse(valid_json);
            CHECK(root != NULL);
            CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, fields[i],
                                                        cJSON_Parse(bad_values[j])));
            json = cJSON_PrintUnformatted(root);
            CHECK(json != NULL);
            CHECK(!parse(json, &sensor));
            cJSON_free(json);
            cJSON_Delete(root);
        }
    }
    const char *bad[] = {"", "{invalid", "[]", "null", "12", "{}", "{\"device_id\":}",
                         "{} trailing", "{\"x\":1,}",
                         "{\"device_id\":\"a\",\"device_id\":\"b\",\"temperature\":1,"
                         "\"humidity\":1,\"battery\":1,\"timestamp\":1}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!parse(bad[i], &sensor));
    const char *error = NULL;
    CHECK(!parse_telemetry(NULL, 1, &sensor, &error));
    CHECK(error != NULL);
    CHECK(!parse_telemetry(valid_json, 0, &sensor, NULL));
    CHECK(!parse_telemetry(valid_json, MAX_JSON, &sensor, NULL));
    char buffer[MAX_JSON];
    memset(buffer, ' ', sizeof(buffer));
    memcpy(buffer, valid_json, strlen(valid_json));
    CHECK(parse_telemetry(buffer, MAX_JSON - 1, &sensor, NULL));
    buffer[10] = '\0';
    CHECK(!parse_telemetry(buffer, MAX_JSON - 1, &sensor, NULL));
}

static void test_device_ids(void)
{
    CHECK(valid_device_id("abc-DEF_012"));
    const char *bad[] = {NULL, "", "a/b", "a+b", "a#b", "a b", "a.b", "a\nb", "\xc3\xa9"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!valid_device_id(bad[i]));
    char id[65];
    memset(id, 'x', 64);
    id[63] = '\0';
    CHECK(valid_device_id(id));
    id[63] = 'x';
    id[64] = '\0';
    CHECK(!valid_device_id(id));
    struct Telemetry sensor;
    CHECK(!parse("{\"device_id\":\"a\\u0000/b\",\"temperature\":1,\"humidity\":1,"
                 "\"battery\":1,\"timestamp\":1}", &sensor));
    CHECK(!parse("{\"device_id\":123,\"temperature\":1,\"humidity\":1,"
                 "\"battery\":1,\"timestamp\":1}", &sensor));
    CHECK(!parse("{\"device_id\":\"bad/id\",\"temperature\":1,\"humidity\":1,"
                 "\"battery\":1,\"timestamp\":1}", &sensor));
}

static void test_numeric_boundaries(void)
{
    struct NumericCase { const char *temp, *hum, *bat, *ts; bool valid; };
    const struct NumericCase cases[] = {
        {"-273.15", "0", "0", "0", true},
        {"1e308", "100", "100", "9007199254740991", true},
        {"1e999", "50", "50", "1", false},
        {"NaN", "50", "50", "1", false},
        {"20", "-0.01", "50", "1", false},
        {"20", "100.01", "50", "1", false},
        {"20", "1e999", "50", "1", false},
        {"20", "50", "-1", "1", false},
        {"20", "50", "101", "1", false},
        {"20", "50", "19.5", "1", false},
        {"20", "50", "1e999", "1", false},
        {"20", "50", "50", "-1", false},
        {"20", "50", "50", "1.5", false},
        {"20", "50", "50", "9007199254740992", false},
        {"20", "50", "50", "1e999", false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char json[512];
        snprintf(json, sizeof(json), "{\"device_id\":\"n\",\"temperature\":%s,"
                 "\"humidity\":%s,\"battery\":%s,\"timestamp\":%s}",
                 cases[i].temp, cases[i].hum, cases[i].bat, cases[i].ts);
        struct Telemetry sensor;
        CHECK(parse(json, &sensor) == cases[i].valid);
    }
}

static void test_storage(void)
{
    struct DeviceStore store = {0};
    for (int i = 0; i < 100; i++) {
        struct Telemetry sensor = {.battery = i, .received_at = {i, 123}};
        snprintf(sensor.device_id, sizeof(sensor.device_id), "device_%d", i);
        CHECK(update_device(&store, &sensor));
        CHECK(store.count == (size_t)i + 1 && store.capacity >= store.count);
    }
    CHECK(store.capacity >= 100);
    for (int i = 0; i < 100; i++)
        CHECK(store.items[i].battery == i && store.items[i].received_at.tv_sec == i);
    struct Telemetry update = store.items[17];
    update.temperature = -25;
    update.timestamp = 1;
    update.received_at = (struct timespec){200, 999};
    CHECK(update_device(&store, &update));
    CHECK(store.count == 100);
    CHECK(store.items[17].temperature == -25 && store.items[17].timestamp == 1);
    CHECK(store.items[17].received_at.tv_sec == 200);
    free_devices(&store);
    CHECK(store.items == NULL && store.count == 0 && store.capacity == 0);
    free_devices(&store);
}

static void test_strict_json_numbers(void)
{
    const char *bad[] = {"01", "00", "1.", "1.e1", "-01", "-00", "0.", "0.e1",
                         "1e", "1e+", "1e-", "1e1.0", ".1", "+1", "--1", "1x", "0x1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (size_t field = 0; field < 4; field++) {
            const char *values[] = {"25", "50", "80", "1"};
            values[field] = bad[i];
            char json[512];
            snprintf(json, sizeof(json), "{\"device_id\":\"new_device\",\"temperature\":%s,"
                     "\"humidity\":%s,\"battery\":%s,\"timestamp\":%s}",
                     values[0], values[1], values[2], values[3]);
            struct Telemetry sensor = {.device_id = "unchanged", .temperature = 17.5,
                .humidity = 60.5, .battery = 42, .timestamp = 123, .received_at = {100, 456}};
            CHECK(!parse(json, &sensor));
            CHECK(strcmp(sensor.device_id, "unchanged") == 0);
            CHECK(sensor.temperature == 17.5 && sensor.humidity == 60.5);
            CHECK(sensor.battery == 42 && sensor.timestamp == 123);
            CHECK(sensor.received_at.tv_sec == 100 && sensor.received_at.tv_nsec == 456);
        }
        char json[512];
        snprintf(json, sizeof(json), "{\"device_id\":\"n\",\"temperature\":25,"
                 "\"humidity\":50,\"battery\":80,\"timestamp\":1,\"extra\":[%s]}", bad[i]);
        struct Telemetry sensor;
        CHECK(!parse(json, &sensor));
    }
    const char *valid[] = {
        "{\"device_id\":\"n\",\"temperature\":-0.25,\"humidity\":8.05e1,\"battery\":2.0e1,\"timestamp\":1E+3}",
        "{\"device_id\":\"n\",\"temperature\":-0,\"humidity\":0,\"battery\":0e-1,\"timestamp\":0.0}",
        "{\"device_id\":\"n\",\"temperature\":1e-2,\"humidity\":1e2,\"battery\":100.0,\"timestamp\":1e3}",
        "{\"device_id\":\"n\",\"temperature\":1.5,\"humidity\":50,\"battery\":80,\"timestamp\":1,"
        "\"extra\":{\"01\":[1.0,-2e+3,0,true,null]}}"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        struct Telemetry sensor;
        CHECK(parse(valid[i], &sensor));
    }
    cJSON *root = cJSON_Parse(valid_json);
    CHECK(root != NULL);
    CHECK(cJSON_AddStringToObject(root, "note", "01 00 1. 1.e1 \"01\" \\") != NULL);
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    struct Telemetry sensor;
    CHECK(parse(json, &sensor));
    CHECK(sensor.battery == 19 && sensor.temperature == 31.5);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void test_thresholds(void)
{
    struct Thresholds thresholds = {30, 80, 20};
    struct Telemetry sensor = {.temperature = 30, .humidity = 80, .battery = 20};
    struct Warnings warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(!warnings.high_temperature && !warnings.high_humidity && !warnings.low_battery);
    sensor.temperature = 30.01;
    warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(warnings.high_temperature && !warnings.high_humidity && !warnings.low_battery);
    sensor.temperature = 30;
    sensor.humidity = 80.01;
    warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(!warnings.high_temperature && warnings.high_humidity && !warnings.low_battery);
    sensor.humidity = 80;
    sensor.battery = 19;
    warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(!warnings.high_temperature && !warnings.high_humidity && warnings.low_battery);
    thresholds = (struct Thresholds){25.5, 70.5, 19.5};
    warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(warnings.high_temperature && warnings.high_humidity && warnings.low_battery);
    thresholds = (struct Thresholds){40, 100, 0};
    warnings = evaluate_thresholds(&sensor, &thresholds);
    CHECK(!warnings.high_temperature && !warnings.high_humidity && !warnings.low_battery);
}

static void test_configuration(void)
{
    double value;
    CHECK(parse_number("-273.15", -DBL_MAX, DBL_MAX, &value) && value == -273.15);
    CHECK(parse_number("0", 0, 100, &value) && value == 0);
    CHECK(parse_number("100", 0, 100, &value) && value == 100);
    CHECK(parse_number("2.05e1", 0, 100, &value) && value == 20.5);
    const char *bad[] = {NULL, "", " ", " 1", "1 ", "1x", "nan", "inf", "-inf", "1e999", "-1", "101"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!parse_number(bad[i], 0, 100, &value));
    unsigned int number;
    CHECK(parse_uint("0", 1, &number) && number == 0);
    CHECK(parse_uint("1", 1, &number) && number == 1);
    CHECK(!parse_uint("2", 1, &number));
    char maximum[32];
    snprintf(maximum, sizeof(maximum), "%d", INT_MAX);
    CHECK(parse_uint(maximum, INT_MAX, &number) && number == INT_MAX);
    const char *bad_int[] = {NULL, "", "-1", "+1", "1.0", "1x", " 1", "1 ", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_int) / sizeof(bad_int[0]); i++)
        CHECK(!parse_uint(bad_int[i], INT_MAX, &number));
}

static void test_timing(void)
{
    struct Telemetry sensor = {.received_at = {100, 900000000}, .timestamp = 9007199254740991LL};
    CHECK(elapsed_seconds(sensor.received_at, sensor.received_at) == 0);
    CHECK(elapsed_seconds(sensor.received_at, (struct timespec){101, 100000000}) == 0);
    CHECK(elapsed_seconds(sensor.received_at, (struct timespec){110, 899999999}) == 9);
    CHECK(!device_inactive(&sensor, (struct timespec){110, 899999999}, 10));
    CHECK(device_inactive(&sensor, (struct timespec){110, 900000000}, 10));
    CHECK(!device_inactive(&sensor, (struct timespec){110, 900000000}, 11));
    CHECK(elapsed_seconds(sensor.received_at, (struct timespec){99, 0}) == 0);
    CHECK(elapsed_seconds(sensor.received_at, (struct timespec){100, 899999999}) == 0);
    CHECK(elapsed_seconds(sensor.received_at, (struct timespec){230, 900000000}) == 130);
    sensor.received_at = (struct timespec){230, 900000000};
    CHECK(!device_inactive(&sensor, (struct timespec){230, 900000000}, 10));
    char formatted[32];
    format_elapsed(0, formatted); CHECK(strcmp(formatted, "0s") == 0);
    format_elapsed(59, formatted); CHECK(strcmp(formatted, "59s") == 0);
    format_elapsed(60, formatted); CHECK(strcmp(formatted, "1m 0s") == 0);
    format_elapsed(130, formatted); CHECK(strcmp(formatted, "2m 10s") == 0);
    format_elapsed(UINT64_MAX, formatted); CHECK(strlen(formatted) < sizeof(formatted));
}

static void test_topics(void)
{
    char topic[128];
    CHECK(make_alert_topic("sensor_01", topic, sizeof(topic)));
    CHECK(strcmp(topic, "iot/devices/sensor_01/alert") == 0);
    size_t size = strlen(topic) + 1;
    CHECK(make_alert_topic("sensor_01", topic, size));
    CHECK(!make_alert_topic("sensor_01", topic, size - 1));
    CHECK(!make_alert_topic("sensor_01", topic, 0));
    CHECK(!make_alert_topic("sensor_01", NULL, sizeof(topic)));
    CHECK(!make_alert_topic("a/b", topic, sizeof(topic)));
    CHECK(!make_alert_topic(NULL, topic, sizeof(topic)));
    char id[MAX_DEVICE_ID];
    memset(id, 'x', sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    CHECK(make_alert_topic(id, topic, sizeof(topic)));
    CHECK(strlen(topic) == strlen("iot/devices//alert") + MAX_DEVICE_ID - 1);
}

int main(void)
{
    test_valid_telemetry();
    test_missing_and_malformed();
    test_device_ids();
    test_numeric_boundaries();
    test_strict_json_numbers();
    test_storage();
    test_thresholds();
    test_configuration();
    test_timing();
    test_topics();
    printf("PASS: 10 unit test groups, %u checks\n", checks);
    return 0;
}
