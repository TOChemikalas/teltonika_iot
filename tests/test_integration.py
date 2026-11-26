#!/usr/bin/env python3
"""Local broker tests. Never stop or change an existing broker or service."""

import json
from collections import Counter
import os
import pty
import re
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)


def wait_for(check, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f'timed out: {description}')


def stop(process, signum=signal.SIGTERM):
    if process.poll() is None:
        process.send_signal(signum)
    return process.wait(timeout=8)


class Fixture:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.processes = []
        self.files = []
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.config = self.directory / 'broker.conf'
        self.config.write_text(f'listener {self.port} 127.0.0.1\n'
                               'allow_anonymous true\npersistence false\n')
        self.broker = None

    def start(self, command, name, **kwargs):
        output = self.directory / f'{name}.out'
        stream = output.open('a')
        self.files.append(stream)
        process = subprocess.Popen(command, stdout=stream, stderr=stream, **kwargs)
        self.processes.append(process)
        return process, output

    def start_broker(self):
        self.broker, output = self.start(
            [shutil.which('mosquitto'), '-c', str(self.config), '-v'], 'broker')

        def ready():
            assert self.broker.poll() is None, output.read_text()
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=0.1):
                    return True
            except OSError:
                return False

        wait_for(ready, 'isolated broker startup')

    def monitor(self, name, *options):
        process, output = self.start(
            ['./iot_display', '-H', '127.0.0.1', '-P', str(self.port), *options], name)
        wait_for(lambda: 'Telemetry subscription granted' in output.read_text(),
                 f'{name} subscription')
        return process, output

    def publish(self, device, qos=0, payload=None, **values):
        if payload is None:
            message = dict(device_id=device, temperature=25, humidity=50,
                           battery=80, timestamp=1795648002)
            message.update(values)
            payload = json.dumps(message).encode()
        subprocess.run(['mosquitto_pub', '-h', '127.0.0.1', '-p', str(self.port),
                        '-q', str(qos), '-t', f'iot/devices/{device}/telemetry', '-n' if payload == b'' else '-s'],
                       input=payload, check=True, timeout=5)

    def close(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                try:
                    stop(process)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for stream in self.files:
            stream.close()


def last_snapshot(output):
    return output.read_text().rsplit('IoT Device Monitor', 1)[-1]


def row(output, device, section='Active devices'):
    snapshot = last_snapshot(output)
    marker = '\n' + section
    if marker not in snapshot:
        return ''
    content = snapshot.split(marker, 1)[1]
    if section == 'Active devices':
        content = content.split('\nInactive devices', 1)[0]
    return next((line for line in content.splitlines()
                 if line.split() and line.split()[0] == device), '')


def inactive_row(output, device):
    return row(output, device, 'Inactive devices')


def alert_lines(output):
    return [line for line in output.read_text().splitlines()
            if line.startswith(('0 iot/devices/', '1 iot/devices/'))]


def rejected_records(log):
    records = []
    for line in log.read_text().splitlines():
        if ' | REJECTED | ' in line:
            match = re.fullmatch(r'(\d+) \| REJECTED \| Topic: (.*?) \| Reason: ([^\r\n]+)', line)
            assert match is not None, line
            records.append((int(match[1]), match[2], match[3]))
    return records


def test_mqtt(fixture, qos, custom=False):
    name = f'qos{qos}' + ('custom' if custom else '')
    log = fixture.directory / f'{name}.log'
    options = ['--qos', str(qos), '--inactive-after', '2', '-o', str(log)]
    if custom:
        options += ['--temp', '25', '--humidity', '60', '--battery', '25']
    monitor, output = fixture.monitor(name, *options)
    subscriber, alerts = fixture.start(
        ['stdbuf', '-oL', 'mosquitto_sub', '-h', '127.0.0.1', '-p', str(fixture.port), '-q', str(qos),
         '-t', 'iot/devices/+/alert', '-F', '%q %t %p', '-d'], f'{name}-alerts')
    wait_for(lambda: 'received SUBACK' in alerts.read_text(), 'alert subscription')
    abnormal = dict(temperature=28, humidity=70, battery=22) if custom else dict(
        temperature=31, humidity=81, battery=19)
    fixture.publish('test01', qos, **abnormal)
    wait_for(lambda: len(alert_lines(alerts)) == 3, 'three threshold alerts')
    records = []
    for line in alert_lines(alerts):
        received_qos, topic, payload = line.split(' ', 2)
        assert received_qos == str(qos) and topic == 'iot/devices/test01/alert', line
        record = json.loads(payload)
        assert record['device_id'] == 'test01' and record['timestamp'] == 1795648002
        records.append(record)
    assert {record['alert'] for record in records} == {
        'HIGH_TEMPERATURE', 'HIGH_HUMIDITY', 'LOW_BATTERY'}
    assert {record['value'] for record in records} == set(abnormal.values())
    wait_for(lambda: 'HIGH_TEMPERATURE' in row(output, 'test01'), 'dashboard warnings')

    fixture.publish('test01', qos, **abnormal)
    wait_for(lambda: len(alert_lines(alerts)) == 6, 'repeated readings publish every alert')

    bounds = dict(temperature=25, humidity=60, battery=25) if custom else dict(
        temperature=30, humidity=80, battery=20)
    fixture.publish('test01', qos, timestamp=1, **bounds)
    wait_for(lambda: 'OK' in row(output, 'test01'), 'exact threshold boundaries')
    time.sleep(0.2)
    assert len(alert_lines(alerts)) == 6, alerts.read_text()
    assert 'Devices: 1' in last_snapshot(output)
    assert '1970-' in row(output, 'test01'), 'simulator timestamp must remain visible'

    before = output.stat().st_size
    rejected_after = int(time.time())
    for bad in [b'{invalid', b'{}', b'[]', b'{}\0', b'x' * 4096,
                b'{"device_id":"bad/id","temperature":1,"humidity":1,"battery":1,"timestamp":1}', b'']:
        fixture.publish('test01', qos, payload=bad)
    wait_for(lambda: 'Invalid device ID' in output.read_text()[before:], 'invalid payload rejection')
    wait_for(lambda: len(rejected_records(log)) == 7, 'rejection file records')
    assert [reason for _, _, reason in rejected_records(log)] == [
        'Invalid JSON object.', 'Missing or duplicate sensor field.', 'Invalid JSON object.',
        'Invalid MQTT payload size or content.', 'Invalid MQTT payload size or content.',
        'Invalid device ID.', 'Empty telemetry payload.']
    assert all(rejected_after <= timestamp <= int(time.time()) and topic == 'iot/devices/test01/telemetry'
               for timestamp, topic, _ in rejected_records(log))
    assert monitor.poll() is None
    assert 'Devices: 1' in last_snapshot(output)

    for index in range(17):
        fixture.publish(f'dynamic_{index}', qos, temperature=20)
    wait_for(lambda: 'Devices: 18' in last_snapshot(output), 'dynamic array expansion')
    wait_for(lambda: inactive_row(output, 'dynamic_16'), 'inactivity without messages')
    assert not row(output, 'dynamic_16')
    wait_for(lambda: 'Active: 0 | Inactive: 18' in last_snapshot(output), 'inactive counts')
    fixture.publish('dynamic_16', qos, temperature=21, timestamp=0)
    wait_for(lambda: 'OK' in row(output, 'dynamic_16'), 'reactivation with an old timestamp')
    assert '0s' in row(output, 'dynamic_16')
    assert 'Devices: 18' in last_snapshot(output)
    assert 'Active: 1 | Inactive: 17' in last_snapshot(output)
    assert not inactive_row(output, 'dynamic_16')

    assert stop(fixture.broker) == 0
    wait_for(lambda: 'Broker: disconnected' in last_snapshot(output), 'broker loss')
    wait_for(lambda: 'STALE (broker offline)' in last_snapshot(output), 'offline stale status')
    assert inactive_row(output, 'dynamic_0') and not row(output, 'dynamic_0')
    fixture.start_broker()
    wait_for(lambda: output.read_text().count('Telemetry subscription granted') >= 2,
             'reconnection and resubscription', timeout=15)
    fixture.publish('reconnected01', qos, temperature=20)
    wait_for(lambda: 'reconnected01' in last_snapshot(output), 'telemetry after reconnect')
    if qos == 1:
        wait_for(lambda: 'Acknowledged: 6' in output.read_text(), 'alert acknowledgment counters')
    assert stop(monitor, signal.SIGINT if qos == 0 else signal.SIGTERM) == 0
    assert '\x1b' not in output.read_text(), 'noninteractive ANSI escape'
    log_text = log.read_text()
    assert log_text.count('Temp:') == 22 and log_text.count('ALERT:') == 6
    logged_alerts = re.findall(r'^(\d+) \| (\S+) \| ALERT: (\w+) \| Value: ([\d.]+)$',
                               log_text, flags=re.MULTILINE)
    published_alerts = []
    for line in alert_lines(alerts):
        record = json.loads(line.split(' ', 2)[2])
        published_alerts.append((str(record['timestamp']), record['device_id'],
                                 record['alert'], f"{record['value']:.2f}"))
    assert Counter(logged_alerts) == Counter(published_alerts)
    assert 'PUBACK' not in log_text and 'Alert PUBACK received' not in output.read_text()
    if qos == 1:
        assert '6 acknowledged, 0 pending at shutdown' in output.read_text()
    assert stop(subscriber) in (0, -signal.SIGTERM)
    print(f'PASS: {name}: alerts, boundaries, malformed input, storage, inactivity, '
          'reconnection, repeated alerts matched to logs, rejection records, graceful shutdown, no ANSI/PUBACK clutter', flush=True)


def test_malformed_numbers(fixture, qos):
    name = f'strict-numbers-qos{qos}'
    log = fixture.directory / f'{name}.log'
    monitor, output = fixture.monitor(name, '--qos', str(qos), '--inactive-after', '3', '-o', str(log))
    subscriber, alerts = fixture.start(
        ['stdbuf', '-oL', 'mosquitto_sub', '-h', '127.0.0.1', '-p', str(fixture.port),
         '-q', str(qos), '-t', 'iot/devices/+/alert', '-F', '%q %t %p', '-d'], f'{name}-alerts')
    wait_for(lambda: 'received SUBACK' in alerts.read_text(), 'strict-number alert subscription')
    fixture.publish('known_number', qos, timestamp=1)
    wait_for(lambda: 'OK' in row(output, 'known_number'), 'initial known reading')
    first_seen = time.monotonic()
    rejected_after = int(time.time())

    def malformed(device, field, spelling):
        values = dict(device_id=device, temperature=99, humidity=99, battery=1, timestamp=999)
        text = json.dumps(values, separators=(',', ':'))
        return text.replace(f'"{field}":{values[field]}', f'"{field}":{spelling}').encode()

    for spelling in ['01', '00', '1.', '1.e1']:
        for field in ['temperature', 'humidity', 'battery', 'timestamp']:
            for device in ['known_number', 'new_number']:
                fixture.publish(device, qos, payload=malformed(device, field, spelling))
    wait_for(lambda: output.read_text().count('Invalid JSON number syntax.') == 32, '32 rejected numbers')
    wait_for(lambda: len(rejected_records(log)) == 32, '32 rejection records')

    fixture.publish('number_guard', qos)
    wait_for(lambda: row(output, 'number_guard'), 'snapshot after rejected messages')
    original = row(output, 'known_number').split()
    assert original[1:4] == ['25.00', '50.00', '80'] and '1970-' in ' '.join(original)
    assert 'Devices: 2 total' in last_snapshot(output)
    assert not row(output, 'new_number') and not inactive_row(output, 'new_number')
    assert log.read_text().count('Temp:') == 2 and 'ALERT:' not in log.read_text()
    assert not alert_lines(alerts)

    while time.monotonic() < first_seen + 2.5:
        time.sleep(0.05)
    fixture.publish('known_number', qos, payload=malformed('known_number', 'battery', '01'))
    wait_for(lambda: output.read_text().count('Invalid JSON number syntax.') == 33, 'late invalid reading')
    wait_for(lambda: inactive_row(output, 'known_number'), 'invalid reading did not reset receipt time',
             timeout=max(0.1, first_seen + 4.7 - time.monotonic()))
    assert not row(output, 'known_number')

    for spelling in ['01', '00', '1.', '1.e1']:
        for device in ['known_number', 'new_number']:
            fixture.publish(device, qos, payload=malformed(device, 'battery', spelling))
    wait_for(lambda: output.read_text().count('Invalid JSON number syntax.') == 41, 'inactive invalid readings')
    fixture.publish('number_guard', qos)
    wait_for(lambda: '0s' in row(output, 'number_guard'), 'fresh state after inactive invalid readings')
    assert inactive_row(output, 'known_number') and not row(output, 'known_number')
    assert 'Devices: 2 total' in last_snapshot(output)
    assert 'Active: 1 | Inactive: 1' in last_snapshot(output)
    assert not alert_lines(alerts) and 'ALERT:' not in log.read_text()

    fixture.publish('known_number', qos, temperature=26, humidity=55, battery=79, timestamp=0)
    wait_for(lambda: '26.00' in row(output, 'known_number'), 'valid reactivation')
    assert not inactive_row(output, 'known_number')
    assert 'Active: 2 | Inactive: 0' in last_snapshot(output)
    assert stop(monitor) == 0
    assert log.read_text().count('Temp:') == 4 and 'ALERT:' not in log.read_text()
    records = rejected_records(log)
    assert len(records) == 41
    assert all(rejected_after <= timestamp <= int(time.time()) and reason == 'Invalid JSON number syntax.'
               for timestamp, _, reason in records)
    assert Counter(topic for _, topic, _ in records) == {
        'iot/devices/known_number/telemetry': 21, 'iot/devices/new_number/telemetry': 20}
    assert not alert_lines(alerts) and 'Alert PUBACK received' not in output.read_text()
    if qos == 1:
        assert '0 acknowledged, 0 pending at shutdown' in output.read_text()
    assert stop(subscriber) in (0, -signal.SIGTERM)
    print(f'PASS: {name}: 41 malformed messages cannot create/update/reactivate devices, '
          'reset age, publish alerts or add valid-telemetry logs; each rejection logged; valid readings reactivate', flush=True)


def test_rejected_logging(fixture):
    log = fixture.directory / 'rejected.log'
    monitor, output = fixture.monitor('rejected-records', '-o', str(log))
    snapshots = output.read_text().count('IoT Device Monitor')
    rejected_after = int(time.time())
    cases = [
        (b'', 'Empty telemetry payload.'),
        (b'{invalid', 'Invalid JSON object.'),
        (b'{}', 'Missing or duplicate sensor field.'),
        (b'{}\0', 'Invalid MQTT payload size or content.'),
        (b'x' * 4096, 'Invalid MQTT payload size or content.'),
        (b'{"device_id":"x","temperature":"25","humidity":50,"battery":80,"timestamp":1}',
         'Incorrectly typed sensor field.'),
        (b'{"device_id":"bad/id","temperature":25,"humidity":50,"battery":80,"timestamp":1}',
         'Invalid device ID.'),
        (b'{"device_id":"x","temperature":25,"humidity":101,"battery":80,"timestamp":1}',
         'Sensor value out of range or not an integer.'),
        (b'{"device_id":"x","temperature":25,"humidity":50,"battery":01,"timestamp":999}',
         'Invalid JSON number syntax.')]
    for payload, _ in cases:
        fixture.publish('reject_record', payload=payload)
    fixture.publish('reject|\\é', payload=b'{}')
    fixture.publish('x' * 300, payload=b'{}')
    wait_for(lambda: len(rejected_records(log)) == 11, 'all rejection record variants')
    records = rejected_records(log)
    assert [reason for _, _, reason in records[:9]] == [reason for _, reason in cases]
    assert all(rejected_after <= timestamp <= int(time.time()) for timestamp, _, _ in records)
    assert all(topic == 'iot/devices/reject_record/telemetry' for _, topic, _ in records[:9])
    assert records[9][1] == r'iot/devices/reject\x7C\x5C\xC3\xA9/telemetry'
    assert records[10][1] == ('iot/devices/' + 'x' * 300 + '/telemetry')[:252] + '...'
    assert len(records[10][1]) == 255
    assert len(log.read_text().splitlines()) == 11
    assert 'Temp:' not in log.read_text() and 'ALERT:' not in log.read_text()
    assert output.read_text().count('IoT Device Monitor') == snapshots
    assert 'Devices: 0 total | Active: 0 | Inactive: 0' in last_snapshot(output)

    fixture.publish('after_rejection', timestamp=1)
    wait_for(lambda: row(output, 'after_rejection'), 'valid telemetry after rejections')
    assert stop(monitor) == 0
    assert '1 | after_rejection | Temp: 25.00 C | Hum: 50.00 % | Battery: 80 %\n' in log.read_text()
    saved_log = log.read_text()
    no_log, no_log_output = fixture.monitor('rejected-no-file')
    fixture.publish('reject_no_log', payload=b'{}')
    wait_for(lambda: 'Missing or duplicate sensor field.' in no_log_output.read_text(), 'rejection without -o')
    fixture.publish('after_no_log')
    wait_for(lambda: row(no_log_output, 'after_no_log'), 'monitor continues without -o')
    assert stop(no_log) == 0
    assert ' | REJECTED | ' not in no_log_output.read_text() and log.read_text() == saved_log
    print('PASS: rejection timestamps/topics/reasons, bounded escaped topics, unchanged valid log format, '
          'no dashboard refresh on rejection, logging optional', flush=True)


def test_json_nul_escape(fixture):
    log = fixture.directory / 'nul-escape.log'
    monitor, output = fixture.monitor('nul-escape', '-o', str(log))

    # A real JSON Unicode NUL escape must be rejected.
    invalid_payload = (
        b'{"device_id":"nul_test","temperature":25,'
        b'"humidity":50,"battery":80,"timestamp":1,'
        b'"note":"\\u0000"}'
    )

    fixture.publish('nul_test', payload=invalid_payload)

    wait_for(
        lambda: len(rejected_records(log)) == 1,
        'real Unicode NUL escape rejected'
    )

    assert 'Devices: 0 total' in last_snapshot(output)

    # An escaped backslash followed by u0000 is valid JSON.
    valid_payload = (
        b'{"device_id":"nul_test","temperature":25,'
        b'"humidity":50,"battery":80,"timestamp":2,'
        b'"note":"\\\\u0000"}'
    )

    fixture.publish('nul_test', payload=valid_payload)

    wait_for(
        lambda: 'nul_test' in last_snapshot(output),
        'literal backslash sequence accepted'
    )

    assert len(rejected_records(log)) == 1
    assert '2 | nul_test | Temp:' in log.read_text()
    assert stop(monitor) == 0

    print('PASS: JSON Unicode NUL escape regression', flush=True)

def test_log_failure(fixture):
    monitor, output = fixture.monitor('log-full', '-o', '/dev/full')
    fixture.publish('full01', temperature=31)
    wait_for(lambda: 'Telemetry log write failed' in output.read_text(), '/dev/full error')
    fixture.publish('full02')
    wait_for(lambda: 'full02' in last_snapshot(output), 'monitor continues after log error')
    assert stop(monitor) == 0
    assert output.read_text().count('Telemetry log write failed') == 1
    rejected, rejected_output = fixture.monitor('rejected-log-full', '-o', '/dev/full')
    fixture.publish('rejected_full', payload=b'{}')
    wait_for(lambda: 'Rejected telemetry log write failed' in rejected_output.read_text(), 'rejection log failure')
    fixture.publish('rejected_full', payload=b'{}')
    fixture.publish('after_rejected_full')
    wait_for(lambda: row(rejected_output, 'after_rejected_full'), 'continue after rejection log failure')
    assert stop(rejected) == 0
    assert rejected_output.read_text().count('Rejected telemetry log write failed') == 1
    assert 'Telemetry log write failed' not in rejected_output.read_text()
    result = subprocess.run(['./iot_display', '-o', str(fixture.directory / 'absent' / 'log')],
                            capture_output=True, timeout=5)
    assert result.returncode == 1 and b'Cannot open log file' in result.stderr
    print('PASS: valid/rejected log write failures disable logging; log open failure exits cleanly', flush=True)


def test_terminal(fixture):
    master, slave = pty.openpty()
    environment = dict(os.environ, TERM='xterm')
    log = fixture.directory / 'tty.log'
    process = subprocess.Popen(['./iot_display', '-H', '127.0.0.1', '-P', str(fixture.port),
                                '--qos', '1', '--inactive-after', '3', '-o', str(log)],
                               stdin=subprocess.DEVNULL, stdout=slave, stderr=slave, env=environment)
    fixture.processes.append(process)
    os.close(slave)
    data = bytearray()

    def read_until(fragment):
        if select.select([master], [], [], 0.05)[0]:
            data.extend(os.read(master, 65536))
        if callable(fragment):
            return b'Press Ctrl+C to exit.' in frame() and fragment()
        return fragment in data

    def frame():
        return bytes(data).rsplit(b'\x1b[H\x1b[J', 1)[-1]

    try:
        wait_for(lambda: read_until(b'Telemetry subscription granted'), 'TTY connection')
        fixture.publish('tty01', 1, temperature=31, humidity=81, battery=19)
        wait_for(lambda: read_until(b'tty01'), 'TTY telemetry')
        wait_for(lambda: read_until(lambda: b'Pending: 0 | Acknowledged: 3' in frame()), 'TTY PUBACK counters')
        wait_for(lambda: read_until(b'1s '), 'TTY elapsed refresh without messages')
        wait_for(lambda: read_until(b'2s '), 'second TTY elapsed refresh')
        wait_for(lambda: read_until(lambda: b'Active: 0 | Inactive: 1' in frame()), 'TTY inactive section')
        active, inactive = frame().split(b'\nInactive devices', 1)
        assert b'tty01' not in active and b'tty01' in inactive
        fixture.publish('tty02', 1)
        wait_for(lambda: read_until(lambda: b'Active: 1 | Inactive: 1' in frame()), 'TTY mixed sections')
        active, inactive = frame().split(b'\nInactive devices', 1)
        assert b'tty02' in active and b'tty01' in inactive and b'tty01' not in active
        fixture.publish('tty01', 1, temperature=31, humidity=81, battery=19)
        wait_for(lambda: read_until(lambda: b'Active: 2 | Inactive: 0' in frame() and
                                    b'Pending: 0 | Acknowledged: 6' in frame()), 'TTY reactivation and PUBACK accounting')
        assert b'\x1b[H\x1b[J' in data
        assert stop(process) == 0
        while select.select([master], [], [], 0.05)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            data.extend(chunk)
        assert b'Alert PUBACK received' not in data
        assert b'6 acknowledged, 0 pending at shutdown' in data
        assert log.read_text().count('Temp:') == 3 and log.read_text().count('ALERT:') == 6
        assert 'PUBACK' not in log.read_text()
    finally:
        os.close(master)
    print('PASS: interactive active/inactive sections, counts, elapsed updates, reactivation, '
          'quiet PUBACK accounting and repeated alert logs', flush=True)


def test_startup_shutdown(fixture):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    process, output = fixture.start(['./iot_display', '-H', '127.0.0.1', '-P', str(port)],
                                     'initial-no-broker')
    time.sleep(0.3)
    if process.poll() is None:
        assert stop(process) == 0
    else:
        assert process.returncode == 1 and 'MQTT connection setup failed' in output.read_text()
    print('PASS: safe startup/shutdown with no broker', flush=True)


def test_systemd(fixture):
    executable = shutil.which('systemd-analyze')
    if executable is None:
        print('SKIP: systemd unit verification (systemd-analyze unavailable)', flush=True)
        return
    unit = fixture.directory / 'teltonika-iot.service'
    unit.write_text((ROOT / 'systemd/teltonika-iot.service').read_text().replace(
        '%h/.local/bin/iot_display', str(ROOT / 'iot_display')))
    result = subprocess.run([executable, '--user', 'verify', str(unit)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    print('PASS: systemd-analyze --user verify (temporary unit with checkout executable)', flush=True)


def test_simulator(fixture):
    interpreter = os.environ.get('SIMULATOR_PYTHON')
    if not interpreter:
        print('SKIP: supplied simulator (set SIMULATOR_PYTHON=venv/bin/python)', flush=True)
        return
    monitor, output = fixture.monitor('simulator-monitor', '--qos', '1')
    # Redirect only runtime connection settings; leave the supplied file unchanged.
    simulator, _ = fixture.start([interpreter, '-c',
        'import sys; import mqtt_simulator as sim; sim.BROKER="127.0.0.1"; '
        'sim.PORT=int(sys.argv[1]); sim.main()', str(fixture.port)], 'simulator')
    wait_for(lambda: all(device in last_snapshot(output)
                        for device in ['sensor_01', 'sensor_02', 'sensor_03']),
             'supplied simulator readings')
    assert stop(simulator, signal.SIGINT) == 0
    assert stop(monitor) == 0
    broker_log = (fixture.directory / 'broker.out').read_text()
    assert any('Received PUBLISH' in line and 'q0' in line and '/sensor_01/telemetry' in line
               for line in broker_log.splitlines())
    print('PASS: unchanged Python simulator publishes QoS 0 to a QoS 1 subscription', flush=True)


def main():
    for command in ['mosquitto', 'mosquitto_pub', 'mosquitto_sub', 'stdbuf']:
        if shutil.which(command) is None:
            raise SystemExit(f'Missing integration dependency: {command}')
    with tempfile.TemporaryDirectory(prefix='teltonika-tests-') as directory:
        fixture = Fixture(directory)
        try:
            fixture.start_broker()
            test_mqtt(fixture, 0)
            test_mqtt(fixture, 1, custom=True)
            test_malformed_numbers(fixture, 0)
            test_malformed_numbers(fixture, 1)
            test_rejected_logging(fixture)
            test_json_nul_escape(fixture)
            test_log_failure(fixture)
            test_terminal(fixture)
            test_startup_shutdown(fixture)
            test_systemd(fixture)
            test_simulator(fixture)
        except BaseException:
            for path in sorted(Path(directory).glob('*.out')):
                print(f'--- {path.name} (last 4000 characters) ---\n{path.read_text()[-4000:]}')
            raise
        finally:
            fixture.close()
    print('PASS: broker integration suite', flush=True)


if __name__ == '__main__':
    main()
