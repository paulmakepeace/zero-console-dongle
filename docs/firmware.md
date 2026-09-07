# Firmware

## What it does

The MBB narrates on its own whenever it is awake: faults with the measured
current, state changes, contactor steps, the config write at key-off, the
hourly wake and 12 V top-up. That stream is a superset of the app's MBB log
(see [mbb-reference.md](mbb-reference.md)), so the first job is to capture it
whole. Everything else is layered on top.

Phase 1, in [../firmware/](../firmware/), with the build and the endpoints in
its [README](../firmware/README.md):

1. **Capture, always.** Every line from UART2 is stamped and appended to a
   file in the module's flash. No commands, no parsing on the ESP32.
2. **One file per MBB session.** A file opens when pin 8 goes high and closes
   five seconds after it goes low. Oldest files are deleted when free space
   runs low. Several days of parking and a few hours of riding fit in the
   flash.
3. **Serve the files over WiFi.** A status page, a JSON list, download and
   delete. `tools/pull-logs.py` on the homelab fetches new files on a
   schedule and deletes each one after the size checks.
4. **A raw TCP console** on port 6638, tee'd into the capture. Transmit is
   allowed only while pin 8 is high, because pin 9 is the MBB's wake pin and
   a high level reboots a sleeping bike, and the transmit pin is attached to
   the UART only while bytes are going out, because a UART idles high and a
   held-high pin 9 keeps the MBB out of deep sleep. Input arriving while the
   MBB sleeps is dropped, not queued.
5. **Time without a battery.** NTP when WiFi is up; otherwise the MBB's own
   stamps, which it prints on most lines, set the clock. Lines carry both.
6. **WiFi by provisioning.** No credentials in the build. With none stored,
   the dongle raises a setup access point and stores what is entered there.

Phase 2, in likely order:

- **Deep sleep between sessions**, woken by pin 8 rising. Pin 8 is wired to
  an RTC-capable GPIO for this. The MBB raises pin 8 on its own hourly wake
  with pin 9 left low, so this captures every wake; see the sleep and wake
  section of [mbb-reference.md](mbb-reference.md).
- **CAN as a second stream.** TWAI in listen-only mode, frames stamped and
  written raw in a candump-style line format for SavvyCAN or a script. Which
  bus is on pins 6 and 14, and at what rate, is the first thing it tells us.
- **Poll a small command set on a slow schedule** while the bike is on: `in`,
  `pdu`, `bms`, `charging`, `faults`, about once a minute. Each response ends
  at the `ZERO MBB>` prompt, which is the frame delimiter. The poller pauses
  while a console client is connected, and never runs while the MBB sleeps.
- **Push instead of pull**, MQTT to the homelab, once pull has proven the
  files.
- **A fixed firmware other owners can flash** and configure from a browser:
  Improv WiFi with esp-web-tools.

## Implementation choice

Arduino framework under PlatformIO, on the pioarduino platform with the
Arduino core 3.x over ESP-IDF 5. Everything in phase 1 is a mature library
there. The hardware is driven through IDF calls where it matters, the UART
driver and the pin control in particular, so the code moves to ESP-IDF or to
Arduino-as-a-component without rewriting if power tuning or the ULP
coprocessor is ever needed. ESPHome was considered and left: the poller, the
console priority and the transmit gating are a custom component anyway, and
its logger and CAN component get in the way.

Development serial access: `pio device monitor`, `idf.py monitor`, or tio on
the DevKit's own USB port. See [console-port.md](console-port.md) for the tio
flags.

## Design rules

- The transmit pin is attached to the UART only while bytes are being sent,
  only while the MBB is awake, and is an input with a pull-down otherwise,
  from the first instruction of boot. This is the one rule that keeps the
  dongle from waking the bike or holding it awake. Every future feature that
  sends anything goes through the same gate.
- The loop task never blocks on a network client. Console output to a client
  that cannot take it is dropped.
- Flash writes happen only after the MBB has been quiet for 3 s, plus at
  session end, with a 15 s bound. A flash erase holds the UART interrupt off for longer
  than the receive FIFO covers, and the precompiled core keeps that
  interrupt out of IRAM. Frunk USB dies at key-off without warning, and the
  phase 2 supply is cut by a switch, so the bound is also the most a power
  cut can lose.
- The sniffer never ACKs or transmits on the bike's bus. TWAI listen-only in
  the driver, and the transceiver's driver input tied recessive in hardware.
- CAN bitrate is unknown. Try 500k, then 250k, then 125k.
