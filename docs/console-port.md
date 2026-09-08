# MBB console port

## Location

A J1962 (OBD-II shape) female socket in the tank storage compartment, on the
midline at the top front, clipped into the liner. It is not either of the two
USB ports in the same compartment.

It is not a real OBD port. Pins 8 and 9 carry a 3.3 V TTL UART; ELM327 dongles
do nothing. Zero's ZDU dealer tool uses the CAN pair on the same connector.

## Serial settings

115200 8N1, no flow control, CR+LF line endings. Prompt is `ZERO MBB>`.

```bash
picocom -b 115200 --omap crcrlf /dev/ttyUSB0
```

Press Enter twice. Console changes generally need `login` (passwords not
public). `config` does not.

On macOS the CP2102N appears as `/dev/cu.usbserial-XXXX`. Use the `cu.` device,
not `tty.`: the `tty.` side waits for carrier detect and picocom hangs on open.

## A legible session

The MBB ends lines with a bare LF, so unmapped output staircases across the
screen, and it wants backspace rather than delete. [`tools/console.sh`](../tools/console.sh) sets the
mappings, finds the adapter, and logs the raw session under `logs/`:

```bash
tools/console.sh
```

By hand, the same thing is:

```bash
picocom -b 115200 --omap crcrlf,delbs --imap lfcrlf --logfile logs/mbb-$(date +%Y-%m-%d_%H%M%S).log /dev/cu.usbserial-0001
```

Unsolicited `DEBUG:` lines interleave with what you type; that is the MBB, not
the terminal. Quit with Ctrl-A then Ctrl-X.

## A timestamped capture

For measuring gaps, such as how long the console runs after key-off or how
long an hourly wake lasts, the picocom log is not enough: it has no
timestamps, and the MBB's own `DEBUG:` stamps do not cover the bare state
lines. [`tools/capture.py`](../tools/capture.py) reads the port, sends nothing, stamps each line
with the local time at its first byte, drops the NULs, and flushes per line:

```bash
tools/capture.py
```

It writes `logs/mbb-capture-DATE_TIME.log`, echoes to the screen, and
reconnects if the adapter goes away. Stop it with Ctrl-C.

tio also works and allows typing:

```bash
tio -b 115200 -t --timestamp-format iso8601 -L --log-file logs/mbb-DATE.log \
    -m INLCRNL,OCRNL,ONLCRNL,ODELBS /dev/cu.usbserial-0001
```

The map flags give the same line endings and backspace as `console.sh`, and
the log file carries the stamps. One catch: tio stamps a line on its first
byte, and the MBB sends its NUL right after the previous line's LF, so each
stamp is really the previous line's end. Across a long silence the first
line after the gap is stamped before the gap, not after. Read gaps from the
next line down, or use `capture.py`, which skips the NULs before stamping.

Other terminals: `idf.py monitor` comes with ESP-IDF and is a good fit for
the DevKit's own USB port once that toolchain is installed. CoolTerm is not
in Homebrew.

## Captures

The raw log carries a NUL after most lines. Clean it before reading:

```bash
tools/log-clean.sh logs/mbb-DATE.log > logs/mbb-DATE.txt
```

There is no event-log export from the console on this firmware revision
(see the command table in [mbb-reference.md](mbb-reference.md)). Bike logs
come from the Zero app (Support, Email bike logs), which sends `.bin` files
for the MBB and BMS.

File types to expect:

| Source                          | Extension                       |
|---------------------------------|---------------------------------|
| Console session capture         | `.log` raw, `.txt` cleaned      |
| Zero app "Email bike logs"      | `.bin`, binary, MBB and BMS     |
| zero-log-parser output          | `.txt`, `.csv`, `.tsv`, `.json`, `.html` |

Everything under `logs/` is ignored by git, as are `.bin`, `.log`, `.txt`,
`.csv` and `.tsv` anywhere in the repo; `.json` and `.html` parser output is
ignored only inside `logs/`. The `version` header, the `bms` snapshot and
the `charging` table print the VIN and serial numbers, so treat any capture
as private.

Parsers: zero-log-parser (zero-motorcycle-community on GitHub) for `.bin` and
console text; zerologs.bike decodes a `.bin` in the browser without upload.

## Pinout

Measured on the MY2020 SR/S, all voltages relative to pin 5.

| Pin | Function                  | Measured                              |
|-----|---------------------------|---------------------------------------|
| 4   | Chassis GND               | continuity to pin 5                   |
| 5   | Signal GND                | continuity to frame                   |
| 6   | CAN-H                     | 3.1 V (live traffic)                  |
| 8   | MBB TX, to adapter RX     | 3.33 V while the console is up, 0 V asleep |
| 9   | MBB RX, from adapter TX; also the hibernation wake pin | 0 V (no pull-up; the adapter drives) |
| 14  | CAN-L                     | 1.85 V (live traffic)                 |
| 16  | +12 V battery             | 13.0 V key off, 13.2 V key on         |

Numbering: looking into the bike's socket face-on with the long edge up, pin 1
is top-left, 8 top-right, 9 bottom-left, 16 bottom-right. The back of a male
plug viewed the same way has the same layout. Trust the moulded numbers on the
plug over any diagram.

Pin 8 is a logic output, never a supply, and pin 9 is the hibernation wake
pin; how they behave and what that means for a dongle are in
[hardware.md](hardware.md).

## Adapters

Any 3.3 V-logic USB-UART works: CP2102/CP2102N (native 3.3 V), or an FT232
board set to 3.3 V. Verify TX idles at about 3.3 V before connecting. A 5 V-only
adapter does not talk to the MBB and risks the RX pin.

A powered adapter's TX idles high, and that level on pin 9 reboots a
sleeping MBB and holds it out of deep sleep, abandoning a 12 V top-up if one
is under way (see [hardware.md](hardware.md)). Connect with the key on if
the natural cycle matters, and unplug the adapter, or at least pin 9, when
done.

On hand: DSD TECH SH-U09B3 (CP2102N, USB-C, header pins), enumerates as cp210x.
Its bottom header reads 5V0, GND, TXD, RXD, RTS, CTS; the side header 3V3, RI,
DCD, DTR, DSR ([back](img/sh-u09b3-back.jpg), [front](img/sh-u09b3-front.jpg)).
For the bike: GND to OBD 5, TXD to OBD 9, RXD to OBD 8, nothing on 5V0 or 3V3.
Wire colours in use: black GND, orange RXD to OBD 8, yellow TXD to OBD 9
([plug end, numbers visible](img/obd-plug-back-wired.jpg)).
There is no level switch or jumper; the only option is an unpopulated solder
pad near the top left, so the I/O runs at the CP2102N's internal 3.3 V. A meter
on TXD confirms it.

Cheap OBD male plugs often omit pins 8 and 9 entirely. The one bought has all
16.

On macOS the CP210x driver sometimes replays its last buffer without end,
tens of kilobytes a second of one repeated line or of NULs, with the
bridge's RX LED flickering, while the board itself is idle or asleep; a
data-grade USB-C cable brings it on more often. It is the host, not the
board: the status page tells the truth, and unplugging and replugging the
USB clears it.

The DevKit's onboard CP2102 can serve as a plain USB-UART with EN tied to GND.
The silkscreen reads backwards for this use: the pin marked TX (GPIO1) is the
CP2102 RXD, so it goes to OBD 8; the pin marked RX (GPIO3) is the CP2102 TXD,
so it goes to OBD 9.
