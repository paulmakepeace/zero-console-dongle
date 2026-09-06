#!/bin/sh
# Open the MBB console legibly and log the session.
#
# Usage: tools/console.sh [DEVICE]
#   DEVICE defaults to the first CP2102 found: /dev/cu.usbserial-* on macOS,
#   /dev/ttyUSB* on Linux.
#
# The MBB ends lines with LF only, expects CR+LF from the terminal, and wants
# backspace rather than delete. The session is logged raw to logs/ beside this
# repo; run tools/log-clean.sh on the file before reading or grepping it.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$repo/logs"

dev=${1:-}
if [ -z "$dev" ]; then
    for candidate in /dev/cu.usbserial-* /dev/ttyUSB*; do
        [ -e "$candidate" ] && dev=$candidate && break
    done
fi
if [ -z "$dev" ]; then
    echo "console.sh: no serial device found; pass one as the first argument" >&2
    exit 2
fi

log="$repo/logs/mbb-$(date +%Y-%m-%d_%H%M%S).log"
echo "console.sh: $dev, logging to $log (Ctrl-A Ctrl-X to quit)" >&2

exec picocom \
    -b 115200 \
    --omap crcrlf,delbs \
    --imap lfcrlf \
    --logfile "$log" \
    "$dev"
