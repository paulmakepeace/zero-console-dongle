#!/bin/sh
# Strip the NUL bytes and carriage returns from a raw console capture and
# trim trailing whitespace, so grep, diff and an editor see plain lines.
#
# Usage: tools/log-clean.sh RAW.log > CLEAN.log
#        tools/log-clean.sh < RAW.log
set -eu

if [ $# -gt 1 ]; then
    echo "usage: log-clean.sh [RAW.log]" >&2
    exit 2
fi

if [ $# -eq 1 ]; then
    exec < "$1"
fi

# Bytes only: a raw capture carries stray bytes from frame errors and power
# transitions, and BSD sed under a UTF-8 locale aborts on them.
LC_ALL=C tr -d '\000\r' | LC_ALL=C sed 's/[[:space:]]*$//'
