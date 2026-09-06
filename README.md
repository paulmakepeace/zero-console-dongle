# Zero console dongle

An ESP32 DevKit housed in a J1962 (OBD-II shape) plug that lives in the tank
storage compartment of a Zero SR/S (MY2020). It gives USB-C and WiFi access to
the MBB serial console, takes key-switched power from the bike, and carries a
passive CAN sniffer. The immediate motivation is clearing the "bulb out" fault
from aftermarket LED turn signals without a dealer visit.

Status: the console is reachable with an interim USB-UART cable, LED mode is
on, and the bulb-out fault is gone with rear LEDs and front incandescents; the
dongle itself is waiting on parts, nothing built or flashed. It will run from
the frunk USB for now; the pin 16 supply is designed but deferred. See
[docs/open-questions.md](docs/open-questions.md) for what is unproven.

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
- [docs/firmware.md](docs/firmware.md): what the firmware does, in order,
  and the Arduino or ESP-IDF direction.
- [docs/sources.md](docs/sources.md): references.
- [firmware/](firmware/): ESPHome draft, kept as a hardware smoke test for
  when the boards arrive. Not compiled.
- [tools/](tools/): `console.sh` opens a legible, logged console session;
  `capture.py` is a read-only capture with a timestamp on every line;
  `log-clean.sh` strips a raw capture for reading.
- `logs/`: session captures, ignored by git because they carry the VIN and
  serial numbers.

## Where this stands

Done: console access proven with the interim cable, LED mode on and the
bulb-out fault gone, the console catalogue and the bike's state captured, the
app logs characterised, the dongle's parts list settled, power settled as the
frunk USB for now.

Next, in order:

1. Leave `tools/capture.py` running with the bike asleep for an hour or
   more. That captures a full hourly wake cycle and measures how long pin 8
   stays live after key-off, which is not known.
2. Read the pin 8 behaviour from that capture. It decides whether the pin 16
   supply with its P-FET key switch in [docs/hardware.md](docs/hardware.md)
   is worth building later; the frunk USB carries the dongle until then.
3. When the DevKits arrive, bench-test CAN first: DevKit on USB, SN65HVD230
   with its termination removed and its TX pin left unconnected, listen-only
   at 500 kbit/s, then 250k and 125k. That identifies the bus on pins 6 and 14.
   The CAN data is the interesting part of the build.
4. Fit the front LEDs, indicate with the console open, and read the current.
   Fit the 80 ohm resistors only if the fault returns.
5. Firmware in Arduino or ESP-IDF: log the console stream continuously, poll
   a small command set, keep an interactive TCP console with priority, ship
   CAN frames raw. See [docs/firmware.md](docs/firmware.md).

## Talking to the console today

With a 3.3 V USB-UART on OBD pins 5 (GND), 8 (MBB TX) and 9 (MBB RX):

```bash
tools/console.sh
```

Press Enter twice for the `ZERO MBB>` prompt. `help` lists commands. `config`
works without a login; most other changes need `login`, whose passwords are not
public.

For a hands-off capture with a timestamp on every line, nothing sent to the
bike:

```bash
tools/capture.py
```
