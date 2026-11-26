# MQTT IoT Device Monitor

A Linux C11 application using libmosquitto and cJSON. It subscribes to
`iot/devices/+/telemetry`, validates telemetry, keeps the latest received reading
for each device in a growing dynamic array, displays readings, and publishes alerts.

## Requirements and build

On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential mosquitto mosquitto-clients libmosquitto-dev libcjson-dev python3 python3-venv
make
```

The default build uses `-std=c11 -Wall -Wextra -Werror -pthread`.
`make clean` removes the application and unit test executable. No service is installed
or enabled by building or testing.

## Start and check the Mosquitto broker service

On a Linux machine with systemd, start Mosquitto and check its status:

```bash
sudo systemctl start mosquitto
sudo systemctl status mosquitto
```

To start the broker automatically at boot, optionally run
`sudo systemctl enable mosquitto`. These are manual deployment instructions;
building and testing the monitor never enables a service. If systemd is unavailable,
run `mosquitto -v` in a separate terminal when port 1883 is free.

## Run and configure

Start an available MQTT broker, then run:

```bash
./iot_display
./iot_display -H localhost -P 1883 -o monitor.log --inactive-after 15 \
    --temp 32.5 --humidity 85 --battery 25 --qos 1
./iot_display -h
```

| Option | Default | Meaning and valid values |
| --- | --- | --- |
| `-H HOST` | `localhost` | Nonempty broker hostname or IP address |
| `-P PORT` | `1883` | Integer from 1 to 65535 |
| `-o FILE` | Disabled | Append valid telemetry, queued alerts, and rejected-telemetry records to a nonempty file path |
| `-h` | | Print help and exit |
| `--inactive-after SECONDS` | `10` | Integer from 1 to `INT_MAX` (2147483647 on the tested system) |
| `--temp VALUE` | `30` | Finite temperature threshold in degrees Celsius |
| `--humidity VALUE` | `80` | Finite percentage threshold from 0 to 100 |
| `--battery VALUE` | `20` | Finite percentage threshold from 0 to 100; fractions are allowed |
| `--qos 0\|1` | `0` | Requested telemetry subscription QoS and alert publication QoS |

Long options also accept `--option=value`. Missing values, invalid numbers,
nonfinite values, out-of-range percentages, unsupported QoS, unknown options, and
unexpected positional arguments are rejected with exit status 1 before connecting.

SIGINT (Ctrl+C) and SIGTERM request graceful shutdown. Exit status is 0 for a
requested shutdown and 1 for configuration, initialization, subscription, or clock
failures. A log-write failure is reported, disables further file logging, and
allows monitoring to continue. A log-open failure prevents startup.

## Dashboard, elapsed time, and inactivity

The header shows the total number of stored devices plus active and inactive
counts. **Active devices** appear in the full, aligned telemetry table: device ID,
temperature, humidity, battery, the original device timestamp, receipt age, and
sensor warnings. **Inactive devices** appear below it in a compact list containing
only each device ID and its elapsed receipt age. Their last readings stay in memory.

Receipt age uses `CLOCK_MONOTONIC`, independently of the JSON timestamp, and is
formatted as `2s`, `15s`, or `2m 10s`. The interactive dashboard refreshes roughly
once per second without new messages. Device updates and rendering use one mutex.

A device moves to the inactive list when its receipt age reaches
`--inactive-after` (default 10 seconds). Valid telemetry automatically returns it
to the active table, even when its JSON timestamp is older than before. Invalid
telemetry never changes stored values, adds a device, or resets its receipt time.
Warnings in the active table still use the configured sensor thresholds; the
compact inactive list omits the stored sensor columns and warnings.

Broker state remains separate from receipt-age classification. During a broker
outage, expired readings stay in the inactive list with a `STALE (broker offline)`
section label. This describes missing observations, not proof of an individual
hardware failure. Inactivity itself does not publish an MQTT sensor alert.

ANSI cursor movement and screen clearing are enabled only when stdout is a TTY
with a nonempty `TERM` other than `dumb`. For redirected output and systemd,
the application emits plain snapshots on telemetry, connection changes, and
inactivity transitions. This keeps logs readable without printing an idle table
every second. No external `clear` command or terminal input is required.

## MQTT messages and thresholds

Telemetry filter: `iot/devices/+/telemetry`.
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

Required fields must occur exactly once. `device_id` must contain 1-63 ASCII
letters, digits, underscores, or hyphens. Temperature must be finite; humidity
must be finite and within 0-100. Battery must be an integer within 0-100.
Timestamp must be a nonnegative integer no greater than 9007199254740991, the
largest consecutively representable integer in cJSON's double representation.
Extra fields are allowed. Payloads must contain 1-4095 bytes and no embedded NUL;
escaped `\u0000` is also rejected because cJSON exposes NUL-terminated strings.
JSON number tokens are checked before cJSON conversion. Leading zeros and missing
fraction/exponent digits are rejected, including `01`, `00`, `1.`, and `1.e1`.
Valid negatives, decimals and scientific notation retain the existing value
validation. Number-like text inside quoted strings is unaffected. Invalid
messages are reported and rejected before storage, receipt-time updates, alerts,
or valid-telemetry logging. With `-o`, each rejected message gets its own concise
record, for example:

```text
1795648002 | REJECTED | Topic: iot/devices/test01/telemetry | Reason: Invalid JSON number syntax.
```

The rejected-record timestamp is the host's current Unix time in seconds, not an
untrusted JSON timestamp. The topic and validation reason are recorded without
the payload. To keep records bounded and on one line, topic control/non-ASCII
bytes, backslashes and `|` are escaped as `\xHH`; the escaped topic is limited to
255 characters, ending with `...` when truncated. Rejection records do not add or
update devices, reset inactivity, refresh the dashboard, or publish MQTT alerts.
The existing stderr validation diagnostic remains available with or without
`-o`. A rejected-record write failure reports the error and disables file logging
while monitoring continues, just like other log-write failures. Valid-telemetry
and alert record formats are unchanged.

Each exceeded threshold generates a separate alert for each valid incoming
message. The dashboard and publisher call the same threshold evaluation function.
Equality does not trigger a warning. With `-o`, every valid received telemetry
message is logged and every successful alert publication is logged as before.
Repeated abnormal readings intentionally produce repeated alerts and log records;
there is no deduplication, throttling, or suppression. PUBACKs do not add file-log
entries.

| Condition | Default | Alert type |
| --- | --- | --- |
| Temperature strictly above `--temp` | > 30 C | `HIGH_TEMPERATURE` |
| Humidity strictly above `--humidity` | > 80% | `HIGH_HUMIDITY` |
| Battery strictly below `--battery` | < 20% | `LOW_BATTERY` |

Alert topic: `iot/devices/<device_id>/alert`.

```json
{"device_id":"sensor_01","alert":"HIGH_TEMPERATURE","value":31,"timestamp":1795648002}
```

### QoS behavior

QoS 0 remains the default and provides delivery without acknowledgment. QoS 1
uses acknowledgment and supports at-least-once delivery on that MQTT hop;
duplicates are possible. It does not guarantee exactly-once processing or that
an alert subscriber has acted on a message.

`--qos` sets both the subscription request and alert publication QoS. The broker's
granted subscription QoS is reported. A subscription at QoS 1 cannot upgrade a
message originally published at QoS 0: the supplied Python simulator still sends
QoS 0 telemetry. Use the Mosquitto CLI tests below for actual QoS 1 telemetry.

For QoS 1, a successful `mosquitto_publish` call queues an alert. The monitor
counts it as pending until its publish callback receives PUBACK, then updates
the pending and acknowledged counters. Routine per-PUBACK messages are not printed
beneath the dashboard. MQTT errors, downgrade reporting, shutdown totals, and
unacknowledged-alert warnings remain visible. File `ALERT:` records mean the
publish call succeeded; they are not delivery confirmations. On shutdown, new
telemetry is ignored while pending alerts get up to three seconds for
acknowledgments if connected. Pending alerts remaining at exit are reported and
may be lost. Reconnection resubscribes using the configured QoS; device state
remains in memory. State and outgoing queues do not survive a process restart.

See the [libmosquitto API documentation](https://mosquitto.org/api/files/mosquitto-h.html#mosquitto_publish_callback_set)
for publish callback semantics.

## Supplied simulator

The provided `mqtt_simulator.py` is unchanged. It publishes three devices
(`sensor_01`, `sensor_02`, `sensor_03`) approximately every two seconds to
`localhost:1883` at QoS 0.

```bash
python3 -m venv venv
venv/bin/pip install 'paho-mqtt>=2.0'
venv/bin/python mqtt_simulator.py
```

Run the broker and monitor in separate terminals. For example, an existing broker
can be started manually with `mosquitto -v` when port 1883 is free.

## Automated tests

### Unit and CLI tests: no broker required

```bash
make test
```

The C tests cover valid telemetry, missing and incorrectly typed fields, malformed
JSON, payload limits and NUL rejection, invalid IDs, numeric boundaries, updates
and array expansion to 100 devices, default and custom thresholds, configuration
number syntax and configuration validation, elapsed time, exact inactivity
boundaries, reactivation, and alert topic generation. Strict-number regressions
cover every numeric telemetry field, nested numbers, valid scientific notation,
and escaped strings containing number-like text. Failures identify the source line and failed expression.
The shell tests exercise the actual CLI, including accepted and rejected options.

### Integration tests: start their own isolated local broker

These require Python 3, Mosquitto broker/client utilities, and `stdbuf` from GNU
coreutils. They use a temporary directory and an available loopback port. They
do not stop an existing broker or install or enable a service.

```bash
make integration-test
# Also exercise the unchanged supplied simulator using its existing virtualenv:
SIMULATOR_PYTHON=venv/bin/python make integration-test
```

The simulator test changes only its broker/port variables in the test process
so it reaches the isolated broker; the simulator file is never edited. Without
`SIMULATOR_PYTHON`, that optional check is explicitly reported as skipped.

Integration coverage includes QoS 0 and actual CLI-published QoS 1 messages,
alert contents and received QoS, PUBACK counts, exact threshold boundaries,
custom thresholds, malformed input, dynamic storage, active/inactive sections and
counts, inactivity and reactivation,
broker loss/restart and resubscription, telemetry/alert file logging,
`/dev/full` log-write recovery, log-open failure, SIGINT/SIGTERM exit status,
TTY periodic refresh, noninteractive output, and startup without a broker.
For both QoS levels, 41 malformed-number messages per run verify that invalid
numbers cannot add devices, change stored readings, reset inactivity timing,
reactivate devices, publish alerts, or add valid-telemetry records. Each rejection
now adds only a separate `REJECTED` record when logging is enabled. Repeated valid abnormal
readings are checked against corresponding MQTT alerts and file-log entries.
The interactive PTY test verifies separate active/inactive sections, reactivation,
acknowledgment counters, and absence of per-PUBACK clutter. Additional cases check
rejection timestamps, topics and reasons; topic escaping/truncation; operation
without `-o`; and recovery from rejected-record write failures using `/dev/full`.

When available, `systemd-analyze --user verify` checks a temporary copy of the
example unit with its executable path pointed at this checkout.

### Recorded verification - 26 November 2026, Ubuntu under WSL

| Check | Actual result |
| --- | --- |
| `make clean`, `make`, `make test` | Passed with C11, `-Wall -Wextra -Werror -pthread` |
| C unit tests | Passed after final corrections: 10 groups, 902 checks |
| CLI tests | Passed: 23 invalid and 2 valid configurations |
| Isolated Mosquitto integration suite | Passed after final corrections: QoS 0/1, reconnect, repeated alerts matched to logs, active/inactive sections, reactivation, terminal and signal checks |
| Malformed-number MQTT regressions | Passed: 41 rejected messages at QoS 0 and 41 at QoS 1; no state, timer, alert, or valid-telemetry-log effects; separate rejection records checked |
| Rejected-telemetry logging | Passed: receipt timestamps/topics/reasons, one record per rejection, bounded topic escaping, optional-off behavior, rejection-log failure recovery; full normal/ASan/UBSan/TSan suites passed |
| Supplied simulator against a QoS 1 subscription | Passed; broker log confirmed telemetry QoS 0 |
| AddressSanitizer + UndefinedBehaviorSanitizer | Passed after final corrections: unit/CLI and complete broker integration suites |
| ThreadSanitizer | Passed after final corrections: unit/CLI and complete broker integration suites; no race reports |
| Supplemental MQTT protocol probes | Passed: connection refusal, subscription rejection, QoS downgrade, delayed PUBACK during shutdown, and bounded missing-PUBACK shutdown |
| systemd unit syntax | Passed using a temporary unit pointing at the built executable |
| Actual systemd installation, boot activation, and restart behavior | Not run; no service installed or enabled |

The supplemental protocol probes were run separately from the regular integration
target. The final executable is the normal build. To repeat sanitizer checks separately:

```bash
make clean
make test CFLAGS='-std=c11 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' \
    LDFLAGS='-fsanitize=address,undefined'
SIMULATOR_PYTHON=venv/bin/python make integration-test
make clean
make
make test
```

## Manual MQTT verification

Start `./iot_display --qos 1 --inactive-after 10 -o monitor.log`. In another terminal:

```bash
mosquitto_sub -h localhost -q 1 -t 'iot/devices/+/alert' -v
```

Publish actual QoS 1 telemetry triggering all three default alerts:

```bash
mosquitto_pub -h localhost -q 1 -t 'iot/devices/test01/telemetry' \
    -m '{"device_id":"test01","temperature":31,"humidity":81,"battery":19,"timestamp":1795648002}'
```

Wait at least ten seconds: elapsed time advances and the device moves from the
active table to the compact inactive list. Its last readings remain stored.
Publish a boundary reading; it should return to the active table with age reset
and `OK`, without new alerts:

```bash
mosquitto_pub -h localhost -q 1 -t 'iot/devices/test01/telemetry' \
    -m '{"device_id":"test01","temperature":30,"humidity":80,"battery":20,"timestamp":1795648003}'
```

Repeat with `--qos 0` and `mosquitto_pub -q 0` for backward compatibility. Use
`make integration-test` for safe, isolated broker restart testing. To inspect
logging, use `tail -n 10 monitor.log`. Stop with Ctrl+C and check `echo $?` (0).
For noninteractive operation, run `./iot_display >monitor.out 2>&1 &`, record
`monitor_pid=$!`, then `kill -TERM "$monitor_pid"; wait "$monitor_pid"; echo $?`.

## systemd deployment (example user service)

The example runs as the logged-in user, without root. It uses the journal for
output, sends SIGTERM on stop, allows ten seconds for cleanup, and restarts on
unexpected failure after five seconds. See [systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
for restart semantics. Neither normal signal-driven shutdown nor `systemctl stop`
is treated as a failure.

Run these commands yourself only when ready to install:

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

`make install` installs only the executable. Binary installation paths are
configurable with `PREFIX`, `BINDIR`, and a packaging `DESTDIR`, for example
`make install PREFIX=/chosen/prefix` or `make install BINDIR=/chosen/bin`.
If you choose another location, edit the copied unit's `ExecStart` to the absolute
binary path before activation. Adjust `EnvironmentFile` similarly to choose a
custom configuration directory. Defaults use systemd's `%h` home-directory
specifier. If using a custom `XDG_CONFIG_HOME`, the command above locates the
unit there while the example environment file still resides under `%h/.config`.
Use an absolute path in a unit; systemd does not expand shell `~` in `ExecStart`.

After changing `monitor.env`, restart the service:

```bash
systemctl --user restart teltonika-iot.service
```

For an optional telemetry/alert file as well as the journal, edit the copied
`ExecStart` line to add `-o /absolute/user-writable/path/monitor.log`, then reload
and restart. The parent directory must exist. No working directory is required.

A user service starts when the user's systemd manager starts, normally at login.
For unattended boot and operation after logout, an administrator may enable
lingering with `sudo loginctl enable-linger "$USER"`. This is optional; it does
not make the monitor run as root. The machine must have systemd and an available
MQTT broker. Broker outages trigger reconnect attempts; an initial connection
setup error can exit and be retried by `Restart=on-failure`.

Remove the service and executable:

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

If you enabled lingering solely for this service and no other user services need
it, an administrator can disable it with `sudo loginctl disable-linger "$USER"`.

## Optional-feature status

| Improvement | Status |
| --- | --- |
| Summary of all known devices | Implemented: active telemetry table, compact inactive list, total/active/inactive counts |
| Time since last telemetry | Implemented with a monotonic receipt clock |
| Inactive device detection | Implemented with configurable timeout and recovery |
| Configurable alert thresholds | Implemented with shared dashboard/publisher evaluation |
| MQTT QoS 1 | Implemented with acknowledgment tracking; QoS 0 remains default |
| Automated unit tests | Implemented with `make test`; broker tests are separate |
| systemd integration | Example user service and deployment/removal instructions provided |

## Known limitations

- Device lookup remains a linear search. Storage is limited by memory, and full
  table snapshots are intended for modest device counts. Long IDs and very wide
  numbers may need a wider terminal.
- The JSON `device_id` is validated but is not cross-checked against the MQTT
  topic's ID, preserving the original behavior.
- Only the latest received reading is retained. Device state, alert queues, and
  inactivity history are not persisted. Old device timestamps are not rejected.
- QoS 1 may duplicate telemetry and alerts. No application-level deduplication or
  durable outgoing spool is provided, and a broker acknowledgment does not prove
  delivery to an alert consumer. Network-loss and process-exit cases can still
  lose unacknowledged alerts.
- The three-second acknowledgment drain is bounded. Supplemental probes verified
  delayed and missing PUBACK shutdown; packet loss and reconnect with outstanding
  alerts have not been tested.
- Authentication and TLS remain outside this local-development application's
  scope. Use a trusted broker/network.
- Timestamp rendering uses the host's local timezone and C library support;
  unrenderable timestamps fall back to their numeric value.
- Integration tests use local loopback TCP and the installed libmosquitto/cJSON.
  Real boot/login service lifecycle, real broker ACL-policy rejection, device-array
  allocation exhaustion, and non-WSL Linux deployments have not been tested here.
- File logs and the systemd journal need external retention/rotation policies.

## Repository contents

```text
iot_display.c                    MQTT callbacks, dashboard, CLI, logging, shutdown
monitor_logic.c / monitor_logic.h Parsing, storage, thresholds, timing, topic helpers
Makefile                         Build, tests, install/uninstall executable
mqtt_simulator.py                Supplied simulator (unchanged)
tests/test_monitor.c             Broker-free C unit tests
tests/test_cli.sh                Broker-free CLI checks
tests/test_integration.py        Isolated local broker and terminal/service checks
systemd/teltonika-iot.service    Example systemd user service
systemd/monitor.env.example      Example service configuration
README.md                        Setup, behavior, verification, deployment, limits
```
