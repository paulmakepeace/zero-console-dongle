# Firmware

Direction: ESPHome.

- `stream_server` external component (oxan/esphome-stream-server) bridges UART2
  to a TCP port, so the MBB console is reachable over WiFi with
  `nc`/`telnet`/`socat`.
- `canbus` with the `esp32_can` (TWAI) platform for the sniffer, frames
  published to MQTT into the homelab.
- Forward UART2 to UART0 as well, so plugging in USB-C gives a straight serial
  console with no network.

## Open design points

- ESPHome's logger owns UART0 by default. Forwarding the MBB console to USB
  means moving the logger (another hardware UART, or `baud_rate: 0`) and adding
  a small custom component to copy bytes both ways. Not written yet.
- Whether ESPHome's `esp32_can` exposes TWAI listen-only mode. If not, a patch
  or a custom component is needed; the sniffer must never ACK or transmit on
  the bike's bus.
- CAN bitrate is unknown. Try 500k, then 250k, then 125k, listen-only.

The draft configuration in [../firmware/](../firmware/) has not been compiled
or flashed.
