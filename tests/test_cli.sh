#!/bin/sh
set -eu

reject() {
    if ./iot_display "$@" >/dev/null 2>&1; then
        echo "FAIL: accepted invalid arguments: $*" >&2
        exit 1
    else
        status=$?
        if [ "$status" -ne 1 ]; then
            echo "FAIL: invalid arguments exited with $status: $*" >&2
            exit 1
        fi
    fi
}

reject --temp nan
reject --temp inf
reject --temp ''
reject --temp 30junk
reject --temp
reject --humidity -1
reject --humidity 100.1
reject --battery -1
reject --battery 101
reject --qos 2
reject --qos 1.0
reject --qos ''
reject --inactive-after 0
reject --inactive-after -1
reject --inactive-after 1.5
reject --inactive-after 2147483648
reject -P 0
reject -P 65536
reject -P ''
reject -H ''
reject -o ''
reject --unknown
reject positional

./iot_display -H localhost -P 65535 -o /unused --temp -273.15 \
    --humidity 0 --battery 100 --inactive-after 2147483647 --qos 1 -h >/dev/null
./iot_display --temp=0 --humidity=100 --battery=0 --qos=0 -h >/dev/null
echo 'PASS: CLI validation (23 invalid, 2 valid configurations; no broker)'
