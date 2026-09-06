# Zero console dongle

An ESP32 DevKit housed in a J1962 (OBD-II shape) plug that lives in the tank
storage compartment of a Zero SR/S (MY2020). It gives USB-C and WiFi access to
the MBB serial console, takes key-switched power from the bike, and carries a
passive CAN sniffer. The immediate motivation is clearing the "bulb out" fault
from aftermarket LED turn signals without a dealer visit.

Status: the console is reachable with an interim USB-UART cable, LED mode is
on, and the bulb-out fault is gone with rear LEDs and front incandescents; the
dongle itself is waiting on parts, nothing built or flashed. See [docs/open-questions.md](docs/open-questions.md) for what is
unproven.

## Layout

- [docs/console-port.md](docs/console-port.md): where the port is, measured
  pinout, serial settings, adapters, a legible picocom session, captures.
- [docs/mbb-reference.md](docs/mbb-reference.md): what the console offers
  without a login. Commands, config table and bitfield, PDU channels, fault
  names, CAN networks.
- [docs/bike.md](docs/bike.md): this bike's board, firmware, fitted options
  and a dated snapshot of its counters.
- [docs/led-signals.md](docs/led-signals.md): how the MBB detects a bulb-out,
  the LED mode's current band, measurements, resistor sizing.
- [docs/hardware.md](docs/hardware.md): the dongle build. Shell, board, wiring,
  key-switched power, parts list.
- [docs/firmware.md](docs/firmware.md): ESPHome direction and open design
  points.
- [docs/sources.md](docs/sources.md): references.
- [firmware/](firmware/): ESPHome configuration. Draft, not yet compiled.
- [tools/](tools/): `console.sh` opens a legible, logged console session;
  `log-clean.sh` strips a raw capture for reading.
- `logs/`: session captures, ignored by git because they carry the VIN and
  serial numbers.

## Talking to the console today

With a 3.3 V USB-UART on OBD pins 5 (GND), 8 (MBB TX) and 9 (MBB RX):

```bash
tools/console.sh
```

Press Enter twice for the `ZERO MBB>` prompt. `help` lists commands. `config`
works without a login; most other changes need `login`, whose passwords are not
public.
