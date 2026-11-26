# Teltonika MQTT IoT Monitor

A Linux command-line application written in C11 using **libmosquitto** and
**cJSON**. It subscribes to `iot/devices/+/telemetry`, validates incoming JSON,
stores the latest received reading for each device in a dynamic array, displays
device status, and publishes sensor alerts.

## Contents

- [Requirements and build](#requirements-and-build)
- [Broker and simulator setup](#broker-and-simulator-setup)
- [Usage](#usage)
- [Dashboard and inactivity](#dashboard-and-inactivity)
- [MQTT messages and alerts](#mqtt-messages-and-alerts)
- [Logging](#logging)
- [Testing and verification](#testing-and-verification)
- [systemd deployment](#systemd-deployment)
- [Optional improvements](#optional-improvements)
- [Known limitations](#known-limitations)
- [Repository contents](#repository-contents)

## Requirements and build

The application requires Linux, GCC, Make, libmosquitto, and cJSON. The broker and
integration tests also use Mosquitto and its command-line clients. Python 3 with
virtual environment support is needed for the supplied simulator.

Install the dependencies on Ubuntu and build from the repository directory:

```bash
sudo apt update
sudo apt install build-essential mosquitto mosquitto-clients libmosquitto-dev libcjson-dev python3 python3-venv
make
```

The executable is `iot_display`. The default compiler flags are
`-std=c11 -Wall -Wextra -Werror -pthread`.

```bash
make clean
```

`make clean` removes the application and unit-test executables. Building or
testing the project does not install or enable a service. Unless noted otherwise,
run the commands below from the repository directory.

## Broker and simulator setup

### Start the Mosquitto broker

On a Linux machine with systemd, start Mosquitto and check its status:

```bash
sudo systemctl start mosquitto
sudo systemctl status mosquitto
```

To start the broker automatically at boot, optionally run
`sudo systemctl enable mosquitto`. These are manual deployment commands. If
systemd is unavailable, run the broker in a separate terminal when port 1883 is
free:

```bash
mosquitto -v
```

### Run the supplied simulator

The supplied `mqtt_simulator.py` publishes telemetry for `sensor_01`, `sensor_02`,
and `sensor_03` approximately every two seconds to `localhost:1883` at QoS 0.
The simulator file is unchanged.

```bash
python3 -m venv venv
venv/bin/pip install 'paho-mqtt>=2.0'
venv/bin/python mqtt_simulator.py
```

Keep the broker, simulator, and monitor in separate terminals.

## Usage

Start the monitor with the defaults:

```bash
./iot_display
```

Specify a broker, enable file logging, and override the optional settings:

```bash
./iot_display -H localhost -P 1883 -o monitor.log --inactive-after 15 \
    --temp 32.5 --humidity 85 --battery 25 --qos 1
```

Display help:

```bash
./iot_display -h
```

### Command-line options

| Option | Default | Description and accepted values |
| --- | --- | --- |
| `-H HOST` | `localhost` | Nonempty broker hostname or IP address. |
| `-P PORT` | `1883` | Integer from 1 to 65535. |
| `-o FILE` | Disabled | Append valid telemetry, queued alerts, and rejected-telemetry records to a nonempty file path. |
| `-h` | — | Print help and exit. |
| `--inactive-after SECONDS` | `10` | Integer from 1 to `INT_MAX` (2147483647 on the tested system). |
| `--temp VALUE` | `30` | Finite temperature threshold in degrees Celsius. |
| `--humidity VALUE` | `80` | Finite percentage threshold from 0 to 100. |
| `--battery VALUE` | `20` | Finite percentage threshold from 0 to 100; fractions are allowed. |
| `--qos 0\|1` | `0` | Requested telemetry subscription QoS and alert publication QoS. |

Long options also accept `--option=value`. Missing values, invalid or nonfinite
numbers, out-of-range percentages, unsupported QoS values, unknown options, and
unexpected positional arguments are rejected with exit status 1 before connecting.

### Shutdown and exit status

SIGINT (Ctrl+C) and SIGTERM request graceful shutdown. Exit status is 0 for a
requested shutdown and 1 for configuration, initialization, subscription, or
clock failures. A log-open failure prevents startup. A log-write failure is
reported, disables further file logging, and allows monitoring to continue.

## Dashboard and inactivity

The header shows the total number of stored devices and the active/inactive
counts. Devices are displayed in two sections:

- **Active devices:** an aligned telemetry table with device ID, temperature,
  humidity, battery, the original device timestamp, elapsed receipt time, and
  sensor warnings.
- **Inactive devices:** a compact list containing each device ID and its elapsed
  receipt time. The last readings remain stored in memory.

Receipt age uses `CLOCK_MONOTONIC`, independently of the JSON timestamp, and is
formatted as `2s`, `15s`, or `2m 10s`. The interactive dashboard refreshes roughly
once per second without new messages. Device updates and rendering use one mutex.

A device moves to the inactive list when its receipt age reaches
`--inactive-after` (default: 10 seconds). Valid telemetry returns it to the active
table, even when its JSON timestamp is older than the previous one. Invalid
telemetry does not create or update devices or reset their receipt time. The
compact inactive list omits stored sensor columns and warnings; active rows use
the configured warning thresholds.

Broker state is displayed separately. During an outage, expired readings remain
in the inactive list under `STALE (broker offline)`. This describes missing
observations, not proof of an individual hardware failure. Inactivity itself
does not publish an MQTT sensor alert.

ANSI cursor movement and screen clearing are enabled only when stdout is a TTY
and `TERM` is nonempty and not `dumb`. Redirected output and systemd receive plain
snapshots on telemetry, connection changes, and inactivity transitions, rather
than an idle table every second. No external `clear` command or terminal input
is required.

## MQTT messages and alerts

### Telemetry

Subscription filter: `iot/devices/+/telemetry`.

Example topic: `iot/devices/sensor_01/telemetry`.

```json
{
  "device_id": "sensor_01",
  "temperature": 31.0,
  "humidity": 81.0,
  "battery": 19,
  "timestamp": 1795648002
}
```

Each required field must occur exactly once. Extra fields are allowed.

| Field | Validation |
| --- | --- |
| `device_id` | String of 1–63 ASCII letters, digits, underscores, or hyphens. |
| `temperature` | Finite number. |
| `humidity` | Finite number from 0 to 100. |
| `battery` | Integer from 0 to 100. |
| `timestamp` | Nonnegative integer no greater than 9007199254740991, the largest consecutively representable integer in cJSON's double representation. |

Payloads must contain 1–4095 bytes and cannot contain raw NUL bytes. Actual JSON
Unicode NUL escapes (`\u0000`) are rejected because cJSON uses NUL-terminated
strings. An escaped backslash (`\\u0000`, representing literal text) is permitted.

JSON number tokens are checked before cJSON conversion. Leading zeros and missing
fraction or exponent digits are rejected, including `01`, `00`, `1.`, and `1.e1`.
Valid negatives, decimals, and scientific notation retain the existing value
validation. Number-like text inside quoted strings is unaffected.

Invalid telemetry is rejected before storage, receipt-time updates, sensor
alerts, or valid-telemetry logging. When file logging is enabled, it produces a
separate rejection record as described in [Logging](#logging).

### Sensor alerts

The dashboard and publisher use the same threshold evaluation. Equality does
not trigger a warning. Each exceeded threshold generates a separate alert for
every valid incoming message.

| Condition | Default | Alert type |
| --- | --- | --- |
| Temperature strictly above `--temp` | > 30 °C | `HIGH_TEMPERATURE` |
| Humidity strictly above `--humidity` | > 80% | `HIGH_HUMIDITY` |
| Battery strictly below `--battery` | < 20% | `LOW_BATTERY` |

Alert topic: `iot/devices/<device_id>/alert`.

```json
{
  "device_id": "sensor_01",
  "alert": "HIGH_TEMPERATURE",
  "value": 31,
  "timestamp": 1795648002
}
```

### QoS behavior

QoS 0 is the default and provides delivery without acknowledgment. QoS 1 uses
acknowledgment and supports at-least-once delivery on that MQTT hop. Duplicates
are possible; QoS 1 does not guarantee exactly-once processing or that an alert
subscriber has acted on a message.

`--qos` sets the subscription request and alert publication QoS. The broker's
granted subscription QoS is reported. A QoS 1 subscription cannot upgrade
telemetry originally published at QoS 0: the supplied simulator still sends QoS 0.
Use `mosquitto_pub -q 1` for actual QoS 1 telemetry testing.

For QoS 1, a successful `mosquitto_publish` call queues an alert. It remains
pending until the publish callback receives PUBACK and updates the pending and
acknowledged counters. Routine per-PUBACK messages are omitted from the dashboard;
MQTT errors, downgrade reporting, shutdown totals, and unacknowledged-alert
warnings remain visible.

On shutdown, new telemetry is ignored while pending alerts receive up to three
seconds for acknowledgments if connected. Any remaining pending alerts are
reported and may be lost. Reconnection resubscribes with the configured QoS and
retains device state in memory. Device state and outgoing queues do not survive
a process restart.

See the [libmosquitto API documentation](https://mosquitto.org/api/files/mosquitto-h.html#mosquitto_publish_callback_set)
for publish callback semantics.

## Logging

Use `-o FILE` to append valid telemetry, successful alert-publication records,
and rejected-telemetry records. Every valid received message is logged. Repeated
abnormal readings intentionally produce repeated alerts and log entries; there
is no deduplication, throttling, or suppression. PUBACKs do not add file-log
entries.

The existing valid-telemetry and alert record formats are unchanged. An `ALERT:`
record means the publish call succeeded; it is not a delivery confirmation.

Rejected messages receive a concise record without the payload:

```text
1795648002 | REJECTED | Topic: iot/devices/test01/telemetry | Reason: Invalid JSON number syntax.
```

The timestamp is the host's current Unix time in seconds, not an untrusted JSON
timestamp. The topic and validation reason are recorded on one line. Topic
control/non-ASCII bytes, backslashes, and `|` are escaped as `\xHH`. The escaped
topic is limited to 255 characters, ending with `...` when truncated.

Rejection records do not create or update devices, reset inactivity, refresh the
dashboard, or publish MQTT alerts. The existing stderr diagnostic remains
available with or without `-o`. Any log-write failure, including a rejection-record
failure, is reported and disables file logging while monitoring continues.

## Testing and verification

### Unit and CLI tests

These tests do not require a broker:

```bash
make test
```

The C tests cover valid telemetry, missing or incorrectly typed fields, malformed
JSON, payload limits and NUL rejection, invalid IDs, numeric boundaries, updates,
dynamic-array expansion to 100 devices, default/custom thresholds, configuration
validation, elapsed time, inactivity boundaries, reactivation, and alert topics.
Strict-number cases cover all numeric fields, nested numbers, valid scientific
notation, and escaped strings containing number-like text. Failures identify the
source line and expression. The shell tests exercise accepted and rejected CLI
configurations.

### Isolated broker integration tests

These tests require Python 3, Mosquitto broker/client utilities, and `stdbuf` from
GNU coreutils. They use a temporary directory and an available loopback port;
they do not stop an existing broker or install or enable a service.

```bash
make integration-test
```

Include the supplied simulator using its existing virtual environment:

```bash
SIMULATOR_PYTHON=venv/bin/python make integration-test
```

The simulator test changes only its broker/port variables in the test process to
reach the isolated broker; the file is never edited. Without `SIMULATOR_PYTHON`,
that optional check is explicitly reported as skipped.

Integration coverage includes QoS 0 and actual QoS 1 CLI traffic, alert contents
and received QoS, PUBACK counts, exact threshold boundaries, custom thresholds,
malformed input, dynamic storage, active/inactive sections and counts,
reactivation, broker restart/resubscription, logging, SIGINT/SIGTERM, periodic TTY
refresh, plain noninteractive output, and startup without a broker.

At each QoS level, 41 malformed-number messages verify that rejected telemetry
cannot add devices, change readings, reset inactivity, reactivate devices,
publish alerts, or add valid-telemetry records. Only separate `REJECTED` records
are added when logging is enabled. Repeated valid alerts are matched to their
file-log entries. Additional cases check rejection timestamps/topics/reasons,
topic escaping/truncation, operation without `-o`, `/dev/full` write failures,
and log-open failures. The PTY test also checks acknowledgment counters and
absence of PUBACK clutter.

When available, `systemd-analyze --user verify` checks a temporary copy of the
example unit with its executable path pointed at the checkout.

The registered Unicode NUL regression checks rejection of an actual `\u0000`
escape and acceptance of `\\u0000` as literal text.

### Recorded verification

The following results were recorded on **26 November 2026, Ubuntu under WSL**.
Unit and integration checks, including the registered Unicode NUL regression,
were confirmed in a temporary source copy during this documentation review.
Sanitizer and supplemental protocol results are recorded development runs,
separate from the normal integration target. The systemd result is a syntax
check, not a deployment-lifecycle test.

| Check | Recorded result |
| --- | --- |
| `make clean`, `make`, `make test` | Passed with C11 and `-Wall -Wextra -Werror -pthread`. |
| C unit tests | Passed: 10 groups, 902 checks. |
| CLI tests | Passed: 23 invalid and 2 valid configurations. |
| Isolated Mosquitto integration suite | Passed: QoS 0/1, reconnection, repeated alerts matched to logs, active/inactive sections, reactivation, terminal behavior, and signals. |
| Malformed-number MQTT regressions | Passed: 41 rejected messages at QoS 0 and 41 at QoS 1; no state, timer, alert, or valid-telemetry-log effects; rejection records checked. |
| Rejected-telemetry logging | Passed: receipt timestamps/topics/reasons, one record per rejection, bounded escaping, optional-off behavior, and write-failure recovery; normal and sanitizer suites passed. |
| Supplied simulator with a QoS 1 subscription | Passed; broker log confirmed telemetry QoS 0. |
| Unicode NUL integration regression | Passed: registered in the integration suite; actual NUL escape rejected and escaped-backslash literal accepted. |
| AddressSanitizer and UndefinedBehaviorSanitizer | Passed: unit/CLI and complete broker integration suites. |
| ThreadSanitizer | Passed: unit/CLI and complete broker integration suites; no race reports. |
| Supplemental MQTT protocol probes | Passed: connection refusal, subscription rejection, QoS downgrade, delayed PUBACK during shutdown, and bounded shutdown without PUBACK. |
| systemd unit syntax | Passed using a temporary unit pointing at the built executable. |
| systemd installation, boot activation, and restart behavior | Not run; no service installed or enabled. |

To repeat the sanitizer checks and then restore the normal build:

```bash
make clean
make test CFLAGS='-std=c11 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' \
    LDFLAGS='-fsanitize=address,undefined'
SIMULATOR_PYTHON=venv/bin/python make integration-test
make clean
make
make test
```

### Manual MQTT verification

With the broker running, start the monitor:

```bash
./iot_display --qos 1 --inactive-after 10 -o monitor.log
```

Subscribe to alerts in another terminal:

```bash
mosquitto_sub -h localhost -q 1 -t 'iot/devices/+/alert' -v
```

Publish QoS 1 telemetry that triggers all three default alerts:

```bash
mosquitto_pub -h localhost -q 1 -t 'iot/devices/test01/telemetry' \
    -m '{"device_id":"test01","temperature":31,"humidity":81,"battery":19,"timestamp":1795648002}'
```

After at least ten seconds without another reading, the device moves from the
active table to the compact inactive list. Its readings remain stored. Publish
the threshold-boundary reading below: it should return to the active table with
age reset and `OK`, without new alerts.

```bash
mosquitto_pub -h localhost -q 1 -t 'iot/devices/test01/telemetry' \
    -m '{"device_id":"test01","temperature":30,"humidity":80,"battery":20,"timestamp":1795648003}'
```

Repeat with monitor option `--qos 0` and publisher option `-q 0` for backward
compatibility. Use the isolated integration suite for broker restart testing.
Inspect the file log with:

```bash
tail -n 10 monitor.log
```

Stop the foreground monitor with Ctrl+C, then immediately run `echo $?` in that
terminal; the expected status is 0. To check noninteractive SIGTERM shutdown:

```bash
./iot_display >monitor.out 2>&1 &
monitor_pid=$!
kill -TERM "$monitor_pid"
wait "$monitor_pid"
echo $?
```

## systemd deployment

### Install and activate the user service

The example runs as the user without root privileges. It sends SIGTERM on stop,
allows ten seconds for cleanup, writes output to the journal, and restarts after
unexpected failure with a five-second delay. Requested normal shutdown and
`systemctl stop` are not failures. See
[systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
for restart semantics.

Run these commands manually only when ready to install:

```bash
make
make install                         # defaults to ~/.local/bin/iot_display
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
config_dir="$HOME/.config/teltonika-iot"
install -d "$unit_dir" "$config_dir"
install -m 0644 systemd/teltonika-iot.service "$unit_dir/teltonika-iot.service"
install -m 0600 systemd/monitor.env.example "$config_dir/monitor.env"
# Edit monitor.env to configure broker, thresholds, inactivity, and QoS.
systemctl --user daemon-reload
systemctl --user enable --now teltonika-iot.service
systemctl --user status teltonika-iot.service
journalctl --user -u teltonika-iot.service -f
```

### Configure paths and logging

`make install` installs only the executable. Override `PREFIX`, `BINDIR`, or the
packaging `DESTDIR` as needed, for example `make install PREFIX=/chosen/prefix`
or `make install BINDIR=/chosen/bin`.

For a custom binary location, edit the copied unit's `ExecStart` to the absolute
path before activation. Change `EnvironmentFile` for a custom configuration
directory. The defaults use systemd's `%h` home-directory specifier. A custom
`XDG_CONFIG_HOME` changes the unit directory in the commands above; the example
environment file still resides under `%h/.config`. Use an absolute executable
path: systemd does not expand shell `~` in `ExecStart`.

After changing `monitor.env`, restart the service:

```bash
systemctl --user restart teltonika-iot.service
```

For a telemetry/alert/rejection file as well as the journal, add
`-o /absolute/user-writable/path/monitor.log` to the copied `ExecStart` line, then
reload and restart. The parent directory must exist. No working directory is
required.

A user service starts with the user's systemd manager, normally at login. For
unattended boot and operation after logout, an administrator may optionally run
`sudo loginctl enable-linger "$USER"`. This does not make the monitor run as root.
The machine needs systemd and an available MQTT broker. Broker outages trigger
reconnection attempts; an initial connection-setup error can exit and be retried
by `Restart=on-failure`.

### Remove the service

```bash
systemctl --user disable --now teltonika-iot.service
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
rm -- "$unit_dir/teltonika-iot.service"
systemctl --user daemon-reload
systemctl --user reset-failed
make uninstall                       # use the same PREFIX/BINDIR as installation
# Optionally remove your configuration file; preserve logs you want to keep:
rm -- "$HOME/.config/teltonika-iot/monitor.env"
```

If lingering was enabled solely for this service and no other user service needs
it, an administrator can run `sudo loginctl disable-linger "$USER"`.

## Optional improvements

| Section 9 improvement | Implementation status |
| --- | --- |
| Summary of all known devices | Active telemetry table, compact inactive list, and total/active/inactive counts. |
| Time since last telemetry | Monotonic receipt clock with periodic interactive updates. |
| Inactive-device detection | Configurable timeout and reactivation on valid telemetry. |
| Configurable alert thresholds | Shared dashboard and publisher evaluation. |
| MQTT QoS 1 | Acknowledgment tracking; QoS 0 remains the default. |
| Automated unit tests | `make test`, with broker integration tests kept separate. |
| systemd integration | Example user service with deployment and removal instructions. |

## Known limitations

- Device lookup is linear. Storage is limited by memory, and full snapshots are
  intended for modest device counts. Long IDs and very wide numbers may need a
  wider terminal.
- Inactive devices are never automatically removed. Every unique device ID
  remains in memory until shutdown, so usage can grow during long-running
  operation.
- The JSON `device_id` is validated but is not cross-checked against the MQTT
  topic's ID.
- Only the latest **received** reading is retained. Old device timestamps are
  not rejected. Device state, alert queues, and inactivity history are not
  persisted across restarts.
- cJSON stores numbers as `double`. Extremely precise or very small literals may
  be rounded or underflow before integer-value validation.
- QoS 1 can duplicate telemetry and alerts. There is no application-level
  deduplication or durable outgoing spool. A broker acknowledgment does not prove
  delivery to an alert consumer; network loss or process exit can lose
  unacknowledged alerts.
- The acknowledgment drain is bounded to three seconds. Supplemental probes
  verified delayed/missing PUBACK shutdown, but packet loss and reconnection with
  outstanding alerts have not been tested.
- Authentication and TLS are outside this local-development application's scope.
  Use a trusted broker and network.
- Timestamps use the host's local timezone and C library support; unrenderable
  values fall back to their numeric representation.
- Integration tests use loopback TCP and the installed libmosquitto/cJSON. Actual
  boot/login service lifecycle, real broker ACL-policy rejection, device-array
  allocation exhaustion, and non-WSL Linux deployment have not been tested.
- File logs and the systemd journal require external retention/rotation policies.

## Repository contents

| File | Purpose |
| --- | --- |
| [iot_display.c](iot_display.c) | MQTT callbacks, dashboard, CLI, logging, and shutdown. |
| [monitor_logic.c](monitor_logic.c), [monitor_logic.h](monitor_logic.h) | Parsing, storage, thresholds, timing, and topic helpers. |
| [Makefile](Makefile) | Build, tests, and executable install/uninstall targets. |
| [mqtt_simulator.py](mqtt_simulator.py) | Supplied telemetry simulator, unchanged. |
| [tests/test_monitor.c](tests/test_monitor.c) | Broker-free C unit tests. |
| [tests/test_cli.sh](tests/test_cli.sh) | Broker-free CLI checks. |
| [tests/test_integration.py](tests/test_integration.py) | Isolated broker, logging, terminal, and service-syntax checks. |
| [systemd/teltonika-iot.service](systemd/teltonika-iot.service) | Example systemd user service. |
| [systemd/monitor.env.example](systemd/monitor.env.example) | Example service configuration. |
| [README.md](README.md) | Setup, usage, verification, deployment, and limitations. |
