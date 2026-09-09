#!/bin/sh
# Run the host test suites: no hardware, seconds. The firmware's Arduino-free
# logic under firmware/test (LittleFS and Arduino stubbed, millis() a variable)
# and the pull script's own suite against an in-process fake dongle. Exits
# non-zero on any failure.
#
# Usage: tools/test.sh
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
pio=${PIO:-$HOME/.platformio/penv/bin/pio}
echo "test: firmware host suite (pio test -e native)"
"$pio" test -d "$repo/firmware" -e native
echo "test: pull script suite (pytest)"
python3 -m pytest "$repo/tools/tests" -q
