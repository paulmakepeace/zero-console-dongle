# Firmware, phase 1

Arduino framework under PlatformIO. Captures the MBB console to flash whenever
the MBB is awake, serves the files over WiFi, and offers a raw TCP console.
Design in [../docs/firmware.md](../docs/firmware.md).

## Wiring

OBD 5 to GND, OBD 8 to D33 (GPIO33, UART2 RX), OBD 9 to TX2 (GPIO17); the
pin numbers are in `config.h`, the table and the phase 2 parts in
[../docs/hardware.md](../docs/hardware.md), and the rule for when pin 9 is
driven in the design rules of [../docs/firmware.md](../docs/firmware.md).
Power from USB-C for the bench and the frunk socket.

## Build, flash, monitor

```bash
~/.platformio/penv/bin/pio run -d firmware
```

```bash
~/.platformio/penv/bin/pio run -d firmware -t upload
```

```bash
cd firmware && ~/.platformio/penv/bin/pio device monitor
```

The monitor has to run from inside `firmware/`; with `-d` its exception
decoder looks for the project in the wrong place.

Later builds go over the air through the status page's upload, or:

```bash
curl -H 'X-Dongle: 1' -F firmware=@firmware/.pio/build/devkit/firmware.bin http://zero-dongle-a12c.local/update
```

[`tools/flash.sh`](../tools/flash.sh) `[HOST ...|all]` does the build, the
upload and the wait for the board to report the new version;
[`tools/status.py`](../tools/status.py) `[HOST ...|all] [--watch N]` prints
one line per board, or only the changes; [`tools/bench.py`](../tools/bench.py)
runs the regression through the adapter on the bench board (roundtrip,
break, poll, storage, sleep, and a light-sleep scenario that sets the grace
short for the run), about four minutes in all, and refuses the bike unit. Board names and addresses both
work; `DONGLE_HOST` and `DONGLE_BOARDS` set the defaults.

## Tests

The logic that does not need a board lives in [`src/pure/`](src/pure/) as plain C++
headers: the MBB stamp parser and the two-stamp agreement rule, the line
framer, the file-name rules, the JSON escaper and the commit accounting.
The modules wrap them; the tests run them on the host:

```bash
~/.platformio/penv/bin/pio test -e native -d firmware
```

The pull script has its own suite against a fake dongle served in-process:

```bash
python3 -m pytest tools/tests
```

What only hardware can prove, the transmit gate and the sleep edge, is
[`tools/bench.py`](../tools/bench.py) on the bench board through the adapter.

Every version bump in `config.h` is an annotated tag `vX.Y.Z` whose body
rolls up the commits since the previous version; `git tag -n99 v0.5.0`
reads one, and `git push --follow-tags` sends them with the branch.

## First boot

Every board names itself `zero-dongle-XXXX`, the last four hex digits of
its MAC, and uses that name for its hostname, mDNS name and setup network,
so several boards can share a network. The bike's unit is
`zero-dongle-a12c`; write the suffix on each board. The setup network's
password is `zero-` plus the last six hex digits of the MAC, printed on the
serial console at boot, and can be replaced from the setup page.

With no WiFi stored the dongle raises the setup network, and keeps raising
it after each sleep until it is provisioned; the setup network counts as
use for its first ten minutes and while someone is on it, after which an
unprovisioned board sleeps like any other. Join it from a
phone, pick the home network and enter its password; the same page takes
the timezone in POSIX form, the NTP server, a new setup password, whether
to sleep between MBB sessions and the poll interval, all stored in flash
and applied at once, no reboot. Sleep is armed only once the bike has
gone `sleep_days` days, three by default, without a 12 V top-up or a
key-on, the signals that say it is being looked after; 0 arms it whenever
the MBB sleeps, and so does the MBB reporting its long-term storage mode
on, the owner's own statement that the bike is parked, until a key-on or
the MBB reporting it off. Armed, the dongle light-sleeps once
the MBB has been asleep for two minutes with nobody using it, for nine
tenths of the time until the MBB's own hourly wake, so it is up a few
minutes early whatever the sleep timer's clock did, and pin 8 rising wakes
it regardless. A host that has not spoken to the dongle since before the
sleep may take a few seconds, once in a while fifteen, to reach it after
the wake while it looks the dongle's address up again; the pull script's
retries cover that. A status check does not count as use, a download, the live
view, the command page or a console client does. If the stored network
refuses the password three times running the setup network comes up again
for ten minutes, after which the retry resumes, since a marginal link can
fail three handshakes too; if the network is out of reach the dongle just
retries every 30 s with no setup network, however long that lasts, and one
failed handshake on a good password raises nothing. If the home network was renamed, hold the DevKit's
BOOT button while powering up and the setup network comes up. The setup
network carries nothing but the setup page: the log server and the console
come up on every join of the home network and go down whenever the setup
network is raised. Capture runs regardless of WiFi state. `POST
/api/wifi/reset` clears the credentials. A forgotten setup password can be
replaced from the home network with `POST /api/settings`.

## Endpoints

| Path                 | Method | What                                      |
|----------------------|--------|-------------------------------------------|
| `/`                  | GET    | status page                               |
| `/api/status`        | GET    | JSON: board name, MAC, firmware version, uptime, boot count and reset reason, awake, pin 8 level, TX attached, last awake and asleep stamps and the awake count, the active file, time and its source and NTP age, WiFi with mDNS and setup-network state, filesystem, dropped lines, the UART's overrun, back-pressure, frame-error and queue-drop counts, console clients and dropped bytes, the pack's state of charge, voltage, current, capacity and temperatures and the bike state from the last poll, the poll interval, the sleep state (armed, storage mode as the MBB last reported it, the count and last wake source), the store's file count and bytes on flash, its compression since boot and the days of space left at that rate, the ESP32's die temperature, the longest pass of each stage of the loop task, heap and stack headroom, watchdog |
| `/logs`              | GET    | JSON list of files with size and active flag, streamed one file at a time |
| `/logs/NAME`         | GET    | the file; 409 while active, 503 when all four readers are busy, 404 if absent |
| `/logs/NAME`         | DELETE | remove it; 409 while active or being read, or for a bad name. The puller never deletes `dict-*` files |
| `/live`              | GET    | the last lines received                    |
| `/update`            | POST   | firmware image as `firmware` in a multipart body; the status page has the form |
| `/api/wifi/reset`    | POST   | forget WiFi and reboot into setup          |
| `/api/settings`      | GET    | JSON: timezone, NTP server, sleep on or off, days unattended before sleeping, poll interval |
| `/api/settings`      | POST   | form fields `tz`, `ntp`, `setup_pass`, `sleep` (0 or 1), `sleep_days` (0 for always), `poll` (seconds, 0 for never), any subset, applied at once, 400 with nothing applied when a value is over its length (tz 63, ntp 64, setup_pass 32 characters); `sleep_grace` (seconds) is a bench knob, applied but not saved |
| `/cmd`               | GET    | tabbed page of the polled command outputs  |
| `/api/cmd`           | GET    | JSON list of the polled commands: age and size of the last good output, whether the last attempt succeeded, age of the last failure |
| `/api/cmd/NAME`      | GET    | the last output of that command, text, with an `X-Age-Seconds` header; 503 until polled, 404 if unknown |
| `/api/cmd/poll`      | POST   | run the batch now                          |

DELETE, `/update`, `/api/wifi/reset`, `POST /api/settings` and `POST /api/cmd/poll` change state and require the header
`X-Dongle: 1`, which a form on another website cannot send from your
browser; the status page and `pull-logs.py` add it, and so does
`curl -H 'X-Dongle: 1'`. There is no other authentication on the home
network. The server serves one client at a time and gives a connection
that has not yet sent its request five seconds before moving on; a browser
tab left open on the status page opens such connections ahead of its
refreshes, and every other client then waits. The tools allow for it with
eight-second timeouts and the pull script with fifteen; close the tab
before timing anything.

TCP console on port 6638, advertised over mDNS as `_zero-console._tcp`:

```bash
nc zero-dongle-a12c.local 6638
```

Enter twice for the prompt. The dongle adds the CR the MBB wants and turns
delete into backspace, so a plain `nc` works. Input is dropped while the MBB
is asleep, and the transmit pin is driven only under the rule in the design
rules of [../docs/firmware.md](../docs/firmware.md). Output a client cannot
take right now is held for it briefly, then dropped with a `[dongle: N
console bytes dropped]` marker once it catches up, so the dongle never
stalls on a client. Two clients take turns at input. A client that closes
is noticed at once and a client that vanishes without closing is found by
TCP keepalive within about 90 s.

## Files

One zlib file per MBB session, `bBBBB-SSS-YYYYMMDD-HHMMSS.log.z` with the
boot count and a sequence number first so that names sort by creation, and
`nosync` in place of the time when the clock was not yet known. Lines are
compressed as they arrive, against the file's own history and against a
dictionary of the lines this bike keeps printing, which the dongle learns
from its sessions and keeps on the flash as `dict-<id>.txt`; the file's
header names the dictionary by id. The file is created when the first
compressed bytes are committed and closed five seconds after pin 8 goes
low, with the trailer. The pull script inflates each one, fetching the
dictionary by id the first time it needs it, and stores the plain `.log`.
A file cut off by a power loss decodes up to its last commit, and the
puller says how many lines it recovered. At 256 KB a session rolls into the next sequence
number; every header carries the board name and `id bBBBB-SSS, part N`,
the id being the first part's boot count and sequence, so parts join by
identity and a pulled file says which board wrote it. A boot count that
failed to save can move a file's sequence number past its id; the id
still joins the parts. Lines the dongle writes
about itself, clock steps and loss markers, never open a file on their
own; they wait for the next session, and may precede its header in the
file. Only if a week of them fills the buffer do they get a file of their
own. Compressed bytes wait in RAM and
reach the flash once the MBB has been quiet for 3 s, or 15 s after its
first waiting line regardless, or when the 4 KB output buffer is full
whatever it is doing, because a flash erase holds the UART interrupt off long enough to
overrun the chip's receive FIFO, and the MBB tends to follow a lone line
with a burst a second later. A power cut loses at most that much. A write
that fails part-way breaks the stream, so that part closes and the session
continues in the next one, with the lines that were waiting counted as
lost. What counts as awake is in the design
rules of [../docs/firmware.md](../docs/firmware.md). Each line carries the dongle's
stamp then the MBB text, the same format as [`tools/capture.py`](../tools/capture.py) once the
clock is known (an uptime stamp `u000016.875` before that). Oldest files go when free space drops
under 96 KB; a file that cannot be deleted is skipped. Lines that cannot be
written are counted in `/api/status` as `dropped_lines`; UART overruns and
frame errors each leave a marker line in the file and a count in the
status; a filesystem that had to be formatted is counted there too. The
pull script reads the status first and warns about any of them.
[`tools/pull-logs.py`](../tools/pull-logs.py) fetches and deletes them from the homelab into
`logs/dongle/NAME/`, one directory per board.

The log area is 896 KB with a 96 KB reserve. With the dictionary learned,
a timeout wake compresses about seven times and costs one 4 KB block, a
parked day about 30 KB, so the area holds about a month of parking
between pulls, and a ride costs about 6 KB an hour. The status reports the files, the bytes, the
ratio since boot and the days of space left at the current rate. The app slots are 1.5 MB each. Changing the partition table needs a
USB flash and formats the log area, which is counted in the status.

The clock comes from NTP while that fix is under six hours old, and from
the MBB's own stamps otherwise. A stamp counts only at the start of a line,
and two consecutive stamps have to agree before the clock moves, so a dump
of old log entries or one corrupted digit cannot move it. Every step is
written into the log: `dongle: clock stepped ... by the MBB`, or `dongle:
clock set from ntp`.

A 120 s task watchdog covers the loop and the capture task and is fed
through long downloads and uploads; a hung task reboots with the reason in
the status and in the next session header.


