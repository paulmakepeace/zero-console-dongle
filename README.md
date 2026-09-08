# Zero console dongle

An ESP32 DevKit housed in a J1962 (OBD-II shape) plug that lives in the tank
storage compartment of a Zero SR/S (MY2020). It gives USB-C and WiFi access to
the MBB serial console, takes power from the bike, and carries a passive CAN
sniffer. The immediate motivation is clearing the "bulb out" fault
from aftermarket LED turn signals without a dealer visit.

Status: LED mode is on and the bulb-out fault is gone with rear LEDs and
front incandescents. The phase 1 firmware runs on a DevKit wired to pins 5
and 8 of the bike, has captured an hourly wake from deep sleep end to end,
and serves the files over WiFi. The MBB's sleep, wake and console-pin
behaviour is characterised and the CAN and buck parts are on order. See
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
- [docs/hardware.md](docs/hardware.md): the dongle build. Shell, board, how
  the console pins behave, wiring, CAN, power, parts list.
- [docs/firmware.md](docs/firmware.md): what phase 1 does, what phase 2
  adds, the Arduino choice and the design rules.
- [docs/sources.md](docs/sources.md): references.
- [firmware/](firmware/): the PlatformIO project, Arduino framework. Its
  README has the wiring, build, first boot and endpoints.
- [tools/](tools/): `console.sh` opens a legible, logged console session;
  `capture.py` is a read-only capture with a timestamp on every line;
  `pull-logs.py` fetches the dongle's files over WiFi; `log-clean.sh` strips
  a raw capture for reading.
- `logs/`: session captures, ignored by git because they carry the VIN and
  serial numbers.

## Where this stands

Done: console access proven, LED mode on and the bulb-out fault gone, the
console catalogue and the bike's state captured, the app logs characterised,
the hourly wake and the console pins characterised, the power design settled
as always-on from pin 16 with the dongle sleeping on pin 8, the phase 1
firmware written, bench-tested, reviewed, and proven on the bike across a
wake from deep sleep with pin 9 open.

Next, in order:

1. Leave the dongle on pins 5 and 8 collecting wakes; the question now is
   what sets the cellular module's schedule, since the 12 V charge rides on
   it.
2. Pin 9 is connected and the console works from the house over `nc` on
   frunk USB; ride and pull the files with `tools/pull-logs.py`.
3. When the CAN and buck parts land, build the second shell with the phase 2
   wiring in [docs/hardware.md](docs/hardware.md), and bench-test CAN first:
   termination removed, driver input tied recessive, listen-only at
   500 kbit/s, then 250k and 125k. That identifies the bus on pins 6 and 14.
4. Fit the front LEDs, indicate with the console open, and read the current.
   Fit the 80 ohm resistors only if the fault returns.
5. Phase 2 firmware: deep sleep on pin 8, CAN as a second stream, the
   poller. See [docs/firmware.md](docs/firmware.md).

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

Connecting a powered adapter to a sleeping bike reboots the MBB and holds it
out of deep sleep for as long as the adapter is attached; see
[docs/hardware.md](docs/hardware.md). With the dongle flashed, the same
console is on the network:

```bash
nc zero-dongle-a12c.local 6638
```
