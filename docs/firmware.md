# Firmware

## What it does

The MBB narrates on its own whenever it is awake: faults with the measured
current, state changes, contactor steps, the config write at key-off, the
hourly wake and 12 V top-up. That stream is a superset of the app's MBB log
(see [mbb-reference.md](mbb-reference.md)), so the first job is to capture it
whole. Everything else is layered on top.

1. **Capture the console stream, always.** Timestamp every line from UART2 and
   ship it. No commands, no parsing on the ESP32. This runs through the
   shutdown window and the hourly wakes, so it also measures both.
2. **Poll a small command set on a slow schedule** while the bike is on:
   `in`, `pdu`, `bms`, `charging`, `faults`, about once a minute. Each response
   ends at the `ZERO MBB>` prompt, which is the frame delimiter. Ship the raw
   response and parse it at home, so what is extracted can change without a
   reflash.
3. **Interactive access with priority.** A TCP console on the WiFi side and a
   passthrough to USB-C. While a client is connected the poller pauses,
   otherwise its commands and the user's interleave at one prompt. This mutex
   is the only real design point.
4. **CAN as a second stream.** TWAI in listen-only mode, frames timestamped
   and shipped raw in a candump-style line format for SavvyCAN or a script.
   Which bus is on pins 6 and 14, and at what rate, is the first thing this
   stream tells us.
5. **Storage is the homelab's job.** A ring buffer in flash rides out a WiFi
   outage; rides away from home are lost, and the app's log still covers
   those.

## Implementation choice

Recommended: a small custom firmware in ESP-IDF or Arduino, four tasks (UART
reader and line framer, TCP console, MQTT publisher with the poll scheduler,
TWAI listener). ESPHome's `stream_server` gives item 3 on day one, but items
2 and 3 together are a custom component anyway, at which point ESPHome is a
wrapper around code being written regardless. Not decided.

The draft ESPHome configuration in [../firmware/](../firmware/) stays as the
quickest way to prove the hardware when the boards arrive: UART2 on GPIO16 and
GPIO17, `stream_server` on port 6638, a 500 kbit/s CAN listener logging frame
IDs. It has not been compiled or flashed.

## Open design points

- ESPHome's logger owns UART0 by default. A USB passthrough under ESPHome
  means moving the logger and adding a component to copy bytes both ways.
- Whether ESPHome's `esp32_can` exposes TWAI listen-only mode. The sniffer must
  never ACK or transmit on the bike's bus; leaving the transceiver's TX pin
  unconnected enforces that in hardware for the first tests.
- CAN bitrate is unknown. Try 500k, then 250k, then 125k.
- Flash writes must happen at safe points, because the key-switched power
  design cuts the supply without warning once the MBB stops talking.
