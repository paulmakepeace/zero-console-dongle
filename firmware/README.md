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

Later builds can go over the air: `pio run -d firmware -t upload --upload-port zero-dongle.local`,
or the form at `http://zero-dongle.local/update`.

## First boot

With no WiFi stored the dongle raises an access point named `zero-dongle`,
password `zerodongle`. Join it from a phone, pick the home network and enter
its password; the dongle stores it and joins. If the stored network is out of
reach at boot, the setup network stays up and the dongle retries the stored
one every 30 s whenever nobody is on the setup network, then shuts the setup
network once joined. Capture runs regardless of WiFi state. `POST
/api/wifi/reset` clears the credentials.

## Endpoints

| Path                 | Method | What                                      |
|----------------------|--------|-------------------------------------------|
| `/`                  | GET    | status page                               |
| `/api/status`        | GET    | JSON: awake, TX attached, last awake and asleep stamps, time and its source, WiFi, space, dropped lines, UART overflows, filesystem state |
| `/logs`              | GET    | JSON list of files with size and active flag |
| `/logs/NAME`         | GET    | the file; refused with 409 while active    |
| `/logs/NAME`         | DELETE | remove it; refused while active            |
| `/live`              | GET    | the last lines received                    |
| `/update`            | GET, POST | firmware upload form and handler        |
| `/api/wifi/reset`    | POST   | forget WiFi and reboot into setup          |

TCP console on port 6638:

```bash
nc zero-dongle.local 6638
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

One file per MBB session, `YYYYMMDD-HHMMSS.log`, created when the first
lines are committed and closed five seconds after pin 8 goes low. Lines wait
in RAM and reach the flash once the MBB has been quiet for 300 ms, or after
5 s or 12 KB regardless, because a flash erase holds the UART interrupt off
long enough to overrun the chip's receive FIFO. A power cut loses at most
that much. Pin 8 has to read high
for three consecutive 20 ms samples, or deliver a byte, before the MBB counts
as awake. Each line carries the dongle's
stamp then the MBB text, the same format as `tools/capture.py`. A session that
starts before the clock is known is named `0000-bBOOT-N.log` and renamed once
the first MBB stamp or NTP arrives. Oldest files go when free space drops
under 96 KB; a file that cannot be deleted is skipped. Lines that cannot be
written are counted in `/api/status` as `dropped_lines`, UART overruns as
`uart_overflows` with a marker line in the file, and a filesystem that had
to be formatted as `fs_formats`. `tools/pull-logs.py` fetches and deletes
them from the homelab.

The state-changing endpoints, firmware upload and WiFi reset, require the
request's Host header to name the dongle, which stops a web page on another
site from driving them through the owner's browser. Everything else is open
on the LAN by design.
