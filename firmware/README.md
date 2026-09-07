# Firmware, phase 1

Arduino framework under PlatformIO. Captures the MBB console to flash whenever
the MBB is awake, serves the files over WiFi, and offers a raw TCP console.
Design in [../docs/firmware.md](../docs/firmware.md).

## Wiring, phase 1

| OBD pin | DevKit pin | Note                                             |
|---------|------------|--------------------------------------------------|
| 5       | GND        |                                                  |
| 8       | D33        | MBB TX. Internal pull-down; RTC-capable for later |
| 9       | TX2 (D17)  | MBB RX and wake pin. Driven only while the MBB is awake |

Power from USB-C for the bench and the frunk socket.

## Build, flash, monitor

```bash
~/.platformio/penv/bin/pio run -d firmware
```

```bash
~/.platformio/penv/bin/pio run -d firmware -t upload
```

```bash
~/.platformio/penv/bin/pio device monitor -d firmware
```

Later builds can go over the air: `pio run -d firmware -t upload --upload-port zero-dongle.local`,
or the form at `http://zero-dongle.local/update`.

## First boot

With no WiFi stored the dongle raises an access point named `zero-dongle`,
password `zerodongle`. Join it from a phone, pick the home network and enter
its password; the dongle stores it and reboots into it. Capture runs
regardless of WiFi state. `POST /api/wifi/reset` clears the credentials.

## Endpoints

| Path                 | Method | What                                      |
|----------------------|--------|-------------------------------------------|
| `/`                  | GET    | status page                               |
| `/api/status`        | GET    | JSON: awake, time and its source, WiFi, space |
| `/logs`              | GET    | JSON list of files with size and active flag |
| `/logs/NAME`         | GET    | the file                                   |
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
asleep, because driving its wake pin would reboot it.

## Files

One file per MBB session, `YYYYMMDD-HHMMSS.log`, opened when pin 8 goes high
and closed five seconds after it goes low. Each line carries the dongle's
stamp then the MBB text, the same format as `tools/capture.py`. A session that
starts before the clock is known is named `0000-bBOOT-N.log` and renamed once
the first MBB stamp or NTP arrives. Oldest files go when free space drops
under 96 KB. `tools/pull-logs.py` fetches and deletes them from the homelab.
