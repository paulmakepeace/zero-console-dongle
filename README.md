# Zero console dongle

An ESP32 DevKit housed in a J1962 (OBD-II shape) plug that lives in the tank
storage compartment of a Zero SR/S (MY2020). It gives USB-C and WiFi access to
the MBB serial console, takes key-switched power from the bike, and carries a
passive CAN sniffer. The immediate motivation is clearing the "bulb out" fault
from aftermarket LED turn signals without a dealer visit.

Status: parts ordered, nothing built or flashed yet. See
[docs/open-questions.md](docs/open-questions.md) for what is unproven.

## Layout

- [docs/console-port.md](docs/console-port.md): where the port is, measured
  pinout, serial settings, which USB-UART adapters work.
- [docs/led-signals.md](docs/led-signals.md): how the MBB detects a bulb-out,
  the console `config` LED mode, and resistor sizing if still needed.
- [docs/hardware.md](docs/hardware.md): the dongle build. Shell, board, wiring,
  key-switched power, parts list.
- [docs/firmware.md](docs/firmware.md): ESPHome direction and open design
  points.
- [docs/sources.md](docs/sources.md): references.
- [firmware/](firmware/): ESPHome configuration. Draft, not yet compiled.

## Talking to the console today

With a 3.3 V USB-UART on OBD pins 5 (GND), 8 (MBB TX) and 9 (MBB RX):

```bash
picocom -b 115200 --omap crcrlf /dev/ttyUSB0
```

Press Enter twice for the `ZERO MBB>` prompt. `help` lists commands. `config`
works without a login; most other changes need `login`, whose passwords are not
public.
