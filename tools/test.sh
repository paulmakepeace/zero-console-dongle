#!/bin/sh
# Run the host test suites: no hardware, seconds. The firmware's Arduino-free
# logic under firmware/test (LittleFS and Arduino stubbed, millis() a variable),
# the pull script's own suite against an in-process fake dongle, and the
# archive service against a temporary archive. Exits non-zero on any failure.
#
# Usage: tools/test.sh
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
pio=${PIO:-$HOME/.platformio/penv/bin/pio}
echo "test: firmware host suite (pio test -e native)"
"$pio" test -d "$repo/firmware" -e native
echo "test: pull script suite (pytest)"
python3 -m pytest "$repo/tools/tests" -q
echo "test: archive service suite (pytest in the server's venv)"
py=$("$repo/tools/mcp/venv.sh")
"$py" -c 'import pytest, httpx' 2>/dev/null || "$py" -m pip install -q -r "$repo/server/requirements-dev.txt"
"$py" -m pytest "$repo/server/tests" -q
