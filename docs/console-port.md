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

## Capturing logs

Record the whole session rather than copying from the terminal:

```bash
picocom -b 115200 --omap crcrlf --logfile logs/mbb-$(date +%F).log /dev/ttyUSB0
```

Gen3 dump commands at the prompt: `eld` or `eventlogdump` for the event log,
`elde` or `eventlogdumpexhaustive` for the most complete text export, `eldd`
for a dump from a given date, `faults` for error information. Each takes an
optional entry count after it. Output is decoded plain text and begins with
"Printing N of M log entries", which says how much was captured.

File types to expect:

| Source                          | Extension                       |
|---------------------------------|---------------------------------|
| Console session capture         | `.log` or `.txt`, plain text    |
| Zero app "Email bike logs"      | `.bin`, binary, MBB and BMS     |
| zero-log-parser output          | `.txt`, `.csv`, `.tsv`, `.json`, `.html` |

Everything under `logs/` and all of those extensions are ignored by git.
Console captures do not reliably carry the VIN, but a dump can include serial
numbers, so treat any capture as private until read.

Parsers: zero-log-parser (zero-motorcycle-community on GitHub) for `.bin` and
console text; zerologs.bike decodes a `.bin` in the browser without upload.

## Pinout

Measured 2026-09-05 on the MY2020 SR/S, all voltages relative to pin 5.

| Pin | Function                  | Measured                              |
|-----|---------------------------|---------------------------------------|
| 4   | Chassis GND               | continuity to pin 5                   |
| 5   | Signal GND                | continuity to frame                   |
| 6   | CAN-H                     | 3.1 V (live traffic)                  |
| 8   | MBB TX, to adapter RX     | 3.33 V key on, 0 V key off            |
| 9   | MBB RX, from adapter TX   | 0 V (no pull-up; the adapter drives)  |
| 14  | CAN-L                     | 1.85 V (live traffic)                 |
| 16  | +12 V battery             | 13.0 V key off, 13.2 V key on         |

Numbering: looking into the bike's socket face-on with the long edge up, pin 1
is top-left, 8 top-right, 9 bottom-left, 16 bottom-right. The back of a male
plug viewed the same way has the same layout. Trust the moulded numbers on the
plug over any diagram.

Pin 8 is a logic output. It is usable as a key-on sense at microamp load, never
as a supply.

## Adapters

Any 3.3 V-logic USB-UART works: CP2102/CP2102N (native 3.3 V), or an FT232
board set to 3.3 V. Verify TX idles at about 3.3 V before connecting. A 5 V-only
adapter does not talk to the MBB and risks the RX pin.

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

The DevKit's onboard CP2102 can serve as a plain USB-UART with EN tied to GND.
The silkscreen reads backwards for this use: the pin marked TX (GPIO1) is the
CP2102 RXD, so it goes to OBD 8; the pin marked RX (GPIO3) is the CP2102 TXD,
so it goes to OBD 9.
