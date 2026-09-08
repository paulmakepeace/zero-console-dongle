#!/bin/sh
# Build the firmware and flash it over the air, then wait for the board to
# come back on the new version.
#
# Usage: tools/flash.sh [HOST ...]
#   HOST is a board name or address; default zero-dongle-a12c.local, and
#   "all" means every board in DONGLE_BOARDS (default: the bike unit and the
#   bench board). DONGLE_HOST sets the default.
#
# Prints fw, boot and reset reason once the board reports the built version
# and a higher boot count; exits 1 if any board does not come back in 120 s.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
boards=${DONGLE_BOARDS:-"zero-dongle-a12c.local zero-dongle-ebdc.local"}
hosts=${*:-${DONGLE_HOST:-zero-dongle-a12c.local}}
[ "$hosts" = all ] && hosts=$boards
pio=${PIO:-$HOME/.platformio/penv/bin/pio}

version=$(sed -n 's/^#define FW_VERSION *"\([^"]*\)".*/\1/p' "$repo/firmware/src/config.h")
echo "flash: building $version"
"$pio" run -d "$repo/firmware" 2>&1 | grep -E 'error|warning: |Flash:|SUCCESS|FAILED' || true
bin="$repo/firmware/.pio/build/devkit/firmware.bin"
[ -f "$bin" ] || { echo "flash: no image at $bin" >&2; exit 1; }

status=0
for name in $hosts; do
    # Resolve once: an mDNS name can take a while to answer again after the
    # reboot, and the DHCP lease keeps the address across it.
    h=$(python3 -c 'import socket,sys;print(socket.getaddrinfo(sys.argv[1],80,socket.AF_INET)[0][4][0])' "$name" 2>/dev/null) || h=$name
    before=$(curl -s -m 5 "http://$h/api/status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["boot"])' 2>/dev/null) \
        || { echo "flash: $name ($h) is not answering; skipped" >&2; status=1; continue; }
    echo "flash: $name at $h (boot $before) <- $version"
    reply=$(curl -s -m 90 -H 'X-Dongle: 1' -F "firmware=@$bin" "http://$h/update") || reply="no reply"
    case "$reply" in ok*) ;; *) echo "flash: $name refused: $reply" >&2; status=1; continue ;; esac
    t=0; ok=0
    while [ $t -lt 120 ]; do
        sleep 3; t=$((t + 3)); printf .
        line=$(curl -s -m 4 "http://$h/api/status" | python3 -c '
import json,sys
s=json.load(sys.stdin); print(s["fw"], s["boot"], s["reset_reason"], "awake" if s["mbb_awake"] else "asleep", "heap_min", s.get("heap_min_free"))' 2>/dev/null) || continue
        set -- $line
        if [ "$1" = "$version" ] && [ "$2" -gt "$before" ]; then echo; echo "flash: $name up after ${t}s: fw $line"; ok=1; break; fi
    done
    [ $ok = 1 ] || { echo; echo "flash: $name did not come back on $version within 120 s" >&2; status=1; }
done
exit $status
