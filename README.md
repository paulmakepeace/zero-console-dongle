# Zero console dongle

An ESP32 DevKit housed in a J1962 (OBD-II shape) plug that lives in the tank
storage compartment of a Zero SR/S (MY2020). It gives USB-C and WiFi access to
the MBB serial console, takes power from the bike, and carries a passive CAN
sniffer. The immediate motivation is clearing the "bulb out" fault
from aftermarket LED turn signals without a dealer visit.

Status: LED mode is on and the bulb-out fault is gone with rear LEDs and
front incandescents. The phase 1 firmware runs on a DevKit wired to pins 5,
8 and 9 of the bike, captures every MBB session including the hourly wakes
from deep sleep, serves the files over WiFi and carries the console onto the
home network. It runs from the frunk USB socket while the bike is on and
from a wall supply while the bike sleeps, until the always-on supply from
pin 16 is built. The MBB's sleep, wake and console-pin behaviour is
characterised and the CAN and buck parts are on order. See
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
- [docs/compression.md](docs/compression.md): how the session files are
  compressed, the learned dictionary, before-and-after figures, flash wear.
- [docs/sources.md](docs/sources.md): references.
- [firmware/](firmware/): the PlatformIO project, Arduino framework. Its
  README has the build, the tests, first boot and the endpoints.
- [tools/](tools/): [`tools/console.sh`](tools/console.sh) opens a legible, logged console session;
  [`tools/capture.py`](tools/capture.py) is a read-only capture with a timestamp on every line;
  [`tools/pull-logs.py`](tools/pull-logs.py) fetches the dongle's files over WiFi; [`tools/status.py`](tools/status.py) is one
  line per board or a watch for changes; [`tools/flash.sh`](tools/flash.sh) builds and flashes over
  the air; [`tools/bench.py`](tools/bench.py) is the regression through the adapter; [`tools/log-clean.sh`](tools/log-clean.sh)
  strips a raw capture for reading; [`tools/check-private.sh`](tools/check-private.sh) is the pre-commit
  gate against the VIN and serials; [`tools/tests/`](tools/tests/) is the pull script's suite.
  The firmware's host tests are under [`firmware/test/`](firmware/test/).
- `logs/`: session captures, ignored by git because they carry the VIN and
  serial numbers. `.private-patterns` at the root, also ignored, holds the
  regexes the pre-commit gate refuses; install the gate once per clone with
  `ln -sf ../../tools/check-private.sh .git/hooks/pre-commit`.

## Next, in order

1. Keep the dongle on the bike collecting wakes, and ride and pull the files
   with [`tools/pull-logs.py`](tools/pull-logs.py). The open question the captures serve is what
   sets the cellular module's schedule, since the 12 V charge rides on it;
   see [docs/open-questions.md](docs/open-questions.md). Leave the bike in
   storage mode across an hourly wake once: that capture shows the wake-time
   line with the mode on, which the sleep trigger keys on.
2. When the CAN and buck parts land, build the second shell with the phase 2
   wiring and bench-test CAN first, both in
   [docs/hardware.md](docs/hardware.md).
3. Fit the front LEDs and answer the LED questions in
   [docs/open-questions.md](docs/open-questions.md).
4. Phase 2 firmware: CAN as a second stream, then the rest of the list in
   [docs/firmware.md](docs/firmware.md).

## Talking to the console

With a 3.3 V USB-UART on OBD pins 5 (GND), 8 (MBB TX) and 9 (MBB RX):

```bash
tools/console.sh
```

For a hands-off capture with a timestamp on every line, nothing sent to the
bike:

```bash
tools/capture.py
```

The port, the prompt, and what an attached adapter does to a sleeping bike
are in [docs/console-port.md](docs/console-port.md). With the dongle
flashed, the same console is on the home network; see the firmware
[README](firmware/README.md).
