# MQTT IoT Device Monitor

A command-line IoT monitoring application written in C. It receives telemetry from multiple simulated devices over MQTT, displays their latest readings in a live terminal table, detects abnormal sensor values, and publishes MQTT alerts.

The application uses **libmosquitto** for MQTT communication and **cJSON** for JSON parsing. It has been developed and tested on Ubuntu under WSL using a local Mosquitto broker and the supplied Python simulator.

## Features

- Connects to an MQTT broker (default `localhost:1883`) and subscribes to `iot/devices/+/telemetry`.
- Processes messages asynchronously and automatically attempts reconnection after the broker disconnects. It subscribes again after each successful connection.
- Validates JSON fields and ignores malformed telemetry without crashing.
- Tracks the latest **received** reading for each device; the device array grows dynamically as new IDs appear.
- Displays device ID, temperature, humidity, battery percentage, timestamp, and current warnings in a compact terminal table.
- Publishes alerts for high temperature, high humidity, and low battery.
- Optionally appends telemetry and alert records to a log file.
- Handles Ctrl+C (SIGINT) and releases MQTT resources, file handles, and allocated memory.

## Requirements

- Linux (tested on Ubuntu in WSL)
- GCC and Make
- Mosquitto MQTT broker and client utilities
- libmosquitto development library
- cJSON development library
- Python 3 with `venv` and `paho-mqtt>=2.0` to run the provided simulator

Install the packages on Ubuntu:

```bash
sudo apt update
sudo apt install build-essential mosquitto mosquitto-clients libmosquitto-dev libcjson-dev python3-full python3-venv
```

## Build

From the repository directory:

```bash
make
```

The Makefile builds the executable `iot_display` using C11 and the required warning flags (`-Wall -Wextra -Werror`). To remove the executable:

```bash
make clean
```

## Start the broker and simulator

Start the Mosquitto broker and check that it is running:

```bash
sudo systemctl enable --now mosquitto
sudo systemctl status mosquitto
```

Set up the Python simulator in a virtual environment (once):

```bash
python3 -m venv venv
source venv/bin/activate
pip install "paho-mqtt>=2.0"
```

Start the provided simulator in its own terminal:

```bash
python mqtt_simulator.py
```

The supplied simulator publishes data from `sensor_01`, `sensor_02`, and `sensor_03`, approximately every two seconds.

## Run the monitor

In another terminal:

```bash
./iot_display
```

Specify a broker and enable logging:

```bash
./iot_display -H localhost -P 1883 -o monitor.log
```

If the terminal dashboard does not redraw correctly at first, clear the terminal once before launching it:

```bash
clear && ./iot_display -o monitor.log
```

Use Ctrl+C to stop the monitor.

| Option | Description |
| --- | --- |
| `-H HOST` | MQTT broker hostname or IP address (default: `localhost`) |
| `-P PORT` | MQTT broker port (default: `1883`) |
| `-o FILE` | Append telemetry and alert entries to a file (optional) |
| `-h` | Show usage information |

The terminal table shows the latest received values and one or more warnings per device. Existing devices are updated in place rather than added again.

## MQTT topics and message format

### Telemetry received

Subscribe filter:

```text
iot/devices/+/telemetry
```

Example topic:

```text
iot/devices/sensor_01/telemetry
```

Example JSON payload:

```json
{
  "device_id": "sensor_01",
  "temperature": 31.0,
  "humidity": 81.0,
  "battery": 19,
  "timestamp": 1795648002
}
```

Each payload must contain `device_id` (string), `temperature` (number), `humidity` (number), `battery` (integer), and `timestamp` (integer Unix timestamp in seconds).

### Alerts published

Alert topic:

```text
iot/devices/<device_id>/alert
```

Example alert payload:

```json
{
  "device_id": "sensor_01",
  "alert": "HIGH_TEMPERATURE",
  "value": 31,
  "timestamp": 1795648002
}
```

| Condition | Alert type |
| --- | --- |
| Temperature > 30 °C | `HIGH_TEMPERATURE` |
| Humidity > 80% | `HIGH_HUMIDITY` |
| Battery < 20% | `LOW_BATTERY` |

Each exceeded threshold generates its own alert for an incoming message. Subscription and alert publishing use MQTT QoS 0.

## Testing

You can verify alert messages in another terminal:

```bash
mosquitto_sub -h localhost -t 'iot/devices/+/alert' -v
```

Publish a message that triggers all three alerts:

```bash
mosquitto_pub -h localhost -t 'iot/devices/test01/telemetry' -m '{"device_id":"test01","temperature":31,"humidity":81,"battery":19,"timestamp":1795648002}'
```

Publish a message at the threshold boundaries (no alerts expected):

```bash
mosquitto_pub -h localhost -t 'iot/devices/test01/telemetry' -m '{"device_id":"test01","temperature":30,"humidity":80,"battery":20,"timestamp":1795648003}'
```

Check malformed JSON handling:

```bash
mosquitto_pub -h localhost -t 'iot/devices/test01/telemetry' -m '{invalid-json'
```

Check dynamic device storage by publishing 12 distinct IDs:

```bash
for i in $(seq 1 12); do
    mosquitto_pub -h localhost -t "iot/devices/test$i/telemetry" \
        -m "{\"device_id\":\"test$i\",\"temperature\":25,\"humidity\":50,\"battery\":80,\"timestamp\":1795648002}"
done
```

To check reconnection, keep the monitor running, then stop and restart the broker in another terminal:

```bash
sudo systemctl stop mosquitto
sudo systemctl start mosquitto
```

To check logging, start with `-o monitor.log`, publish telemetry, and inspect the file:

```bash
tail -n 10 monitor.log
```

For a controlled log-write failure test on Linux, run `./iot_display -o /dev/full` and publish telemetry. `/dev/full` reports a write failure; the intended behavior is to report the error, disable logging, and continue monitoring. **This failure-path test still needs final confirmation.**

Stop the monitor with Ctrl+C and immediately check its exit status:

```bash
echo $?
```

### Results observed during development

| Scenario | Result |
| --- | --- |
| Clean build with GCC `-std=c11 -Wall -Wextra -Werror` | Passed |
| Connection to local Mosquitto broker | Passed |
| Simulator readings from three devices | Passed |
| Tracking 16 devices, including more than the initial 8 allocated slots | Passed |
| Latest readings updated without adding duplicate device IDs | Passed |
| All three warning types and alert MQTT messages | Passed |
| Optional telemetry and alert logging to `monitor.log` | Passed |
| Invalid JSON handling | Passed in earlier testing; recheck after final changes if needed |
| Broker disconnection and reconnection | Passed in earlier testing; recheck after final changes if needed |
| Ctrl+C shutdown | Observed; final exit code not recorded |
| Simulated log-write failure using `/dev/full` | Not yet confirmed |

## Known limitations

- The live dashboard uses ANSI terminal control sequences and works best in a sufficiently wide Linux terminal. Depending on the terminal, clearing it once before launch may improve rendering.
- The dashboard redraws all tracked devices after every received message. It is intended for modest device counts, not a large production fleet.
- Device lookup is a linear search; storage expands dynamically but is ultimately limited by available memory.
- Device IDs are limited to 63 characters and may use letters, digits, underscores, and hyphens.
- Incoming JSON payloads are limited to 4095 bytes, leaving one byte for the C string terminator.
- The JSON `device_id` is validated but is not cross-checked against the device ID in the MQTT topic.
- The application retains only the latest **received** reading for each device in memory; it does not persist device state across restarts or reject old timestamps.
- MQTT QoS 0 does not guarantee alert delivery.
- Authentication and TLS are not configured by this application; the default setup is for a local development broker.
- Logging is optional. If a log write fails, the program reports the error and disables further logging while continuing to monitor devices.

## Repository contents

```text
iot_display.c       C source code
Makefile            Build and clean targets
README.md           Project setup, usage, tests, and limitations
mqtt_simulator.py   Supplied Python telemetry simulator
```
