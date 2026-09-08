# Firmware, phase 1

Arduino framework under PlatformIO. Captures the MBB console to flash whenever
the MBB is awake, serves the files over WiFi, and offers a raw TCP console.
Design in [../docs/firmware.md](../docs/firmware.md).

## Wiring, phase 1

| OBD pin | DevKit pin | Note                                             |
|---------|------------|--------------------------------------------------|
| 5       | GND        |                                                  |
| 8       | D33        | MBB TX. Internal pull-down; RTC-capable for later |
| 9       | TX2 (D17)  | MBB RX and wake pin. Driven only while a command is being sent, and only while the MBB is awake |

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

## First boot

Every board names itself `zero-dongle-XXXX`, the last four hex digits of
its MAC, and uses that name for its hostname, mDNS name and setup network,
so several boards can share a network. The bike's unit is
`zero-dongle-a12c`; write the suffix on each board. The setup network's
password is `zero-` plus the last six hex digits of the MAC, printed on the
serial console at boot, and can be replaced from the setup page.

With no WiFi stored the dongle raises the setup network. Join it from a
phone, pick the home network and enter its password; the same page takes
the timezone in POSIX form, the NTP server and a new setup password, all
stored in flash. If the stored network fails for a wrong password the setup
network comes up again; if it fails because the network is out of reach the
dongle just retries every 30 s with no setup network, however long that
lasts. If the home network was renamed, hold the DevKit's BOOT button while
powering up and the setup network comes up. The setup network carries nothing
but the setup page: the log server and the console exist only on the home
network. Capture runs regardless of WiFi state. `POST /api/wifi/reset`
clears the credentials.

## Endpoints

| Path                 | Method | What                                      |
|----------------------|--------|-------------------------------------------|
| `/`                  | GET    | status page                               |
| `/api/status`        | GET    | JSON: awake, TX attached, last awake and asleep stamps, time and its source and NTP age, WiFi, filesystem, dropped lines, the UART's overrun, back-pressure, frame-error and queue-drop counts, watchdog and reset reason |
| `/logs`              | GET    | JSON list of files with size and active flag |
| `/logs/NAME`         | GET    | the file; refused with 409 while active    |
| `/logs/NAME`         | DELETE | remove it; refused while active            |
| `/live`              | GET    | the last lines received                    |
| `/update`            | POST   | firmware image as `firmware` in a multipart body; the status page has the form |
| `/api/wifi/reset`    | POST   | forget WiFi and reboot into setup          |

DELETE, `/update` and `/api/wifi/reset` change state and require the header
`X-Dongle: 1`, which a form on another website cannot send from your
browser; the status page and `pull-logs.py` add it, and so does
`curl -H 'X-Dongle: 1'`. There is no other authentication on the home
network.

TCP console on port 6638:

```bash
nc zero-dongle-a12c.local 6638
```

Enter twice for the prompt. The dongle adds the CR the MBB wants and turns
delete into backspace, so a plain `nc` works. Input is dropped while the MBB is
asleep, because driving its wake pin would reboot it. The transmit pin is
attached to the UART only while bytes are being sent and for two seconds
after, then returns to a pulled-down input: a UART idles high, and a high on
pin 9 holds the MBB out of deep sleep. The GPIO is put in that safe state
before anything else runs at boot. Output a client cannot take right now is
held for it briefly, then dropped with a `[dongle: N console bytes dropped]`
marker once it catches up, so the dongle never stalls on a client. Clients
that vanish without closing are found by TCP keepalive within a minute.

## Files

One file per MBB session, `bBBBB-SS-YYYYMMDD-HHMMSS.log` with the boot
count and a sequence number first so that names sort by creation, and
`nosync` in place of the time when the clock was not yet known. A file is
created when the first lines are committed and closed five seconds after
pin 8 goes low, or rolled into the next sequence number at 256 KB, in which
case the first file ends with `session continues in the next file` and the
next one's header says which file it continued from. Lines wait
in RAM and reach the flash once the MBB has been quiet for 3 s, or after
15 s or 12 KB regardless, because a flash erase holds the UART interrupt off
long enough to overrun the chip's receive FIFO, and the MBB tends to follow
a lone line with a burst a second later. A power cut loses at most that
much. Pin 8 has to read high
for three consecutive 20 ms samples, or deliver a byte, before the MBB counts
as awake. Each line carries the dongle's
stamp then the MBB text, the same format as `tools/capture.py`. A session that
starts before the clock is known is named `0000-bBOOT-N.log` and renamed once
the first MBB stamp or NTP arrives. Oldest files go when free space drops
under 96 KB; a file that cannot be deleted is skipped. Lines that cannot be
written are counted in `/api/status` as `dropped_lines`; UART overruns and
frame errors each leave a marker line in the file and a count in the
status; a filesystem that had to be formatted is counted there too. `tools/pull-logs.py` fetches and deletes
them from the homelab.

The log area is 896 KB: a timeout wake is 6 KB, a ride about 40 KB an hour,
so parked days cost about 150 KB and the area holds six of them between
pulls. The app slots are 1.5 MB each. Changing the partition table needs a
USB flash and formats the log area, which is counted in the status.

The clock comes from NTP while that fix is under six hours old, and from
the MBB's own stamps otherwise. A stamp counts only at the start of a line,
and two consecutive stamps have to agree before the clock moves, so a dump
of old log entries or one corrupted digit cannot move it. Every step is
written into the log as a `dongle: clock stepped` line.

A 120 s task watchdog covers the loop and the capture task and is fed
through long downloads and uploads; a hung task reboots with the reason in
the status and in the next session header.

The state-changing endpoints, firmware upload and WiFi reset, require the
request's Host header to name the dongle, which stops a web page on another
site from driving them through the owner's browser. Everything else is open
on the LAN by design.
