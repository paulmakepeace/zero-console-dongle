# Dongle hardware

## Shell

OBD-II male plug shell, about 72 x 46 mm outside. Four posts about 35 x 30 mm
centre to centre, about 6 mm diameter, with the case screws running through
them. About 28 x 24 mm clear between posts. Cable-exit notch at the rear.

## Board

AITRIP 30-pin ESP32 DevKit V1 (WROOM-32, onboard CP2102, USB-C), about
52 x 28 mm.

Plan: trim plastic around one post, mount a board corner hole over another
post, leaving three case screws, one of which anchors the board. Remove or clip
the header pins for height. USB-C receptacle out through the notch, filed to
about 9 x 3.5 mm; check the plug overmould seats.

## Wiring

| OBD pin | Goes to                                                      |
|---------|--------------------------------------------------------------|
| 5       | GND                                                          |
| 8       | ESP32 UART2 RX (GPIO16 default; any GPIO via the matrix)     |
| 9       | ESP32 UART2 TX (GPIO17 default)                              |
| 16      | fuse 0.5 to 1 A, optional TVS, key-switched buck, 5 V to VIN |
| 6       | SN65HVD230 CANH                                              |
| 14      | SN65HVD230 CANL                                              |

CAN transceiver: 3V3 from the DevKit, RS to GND, CTX/CRX to two GPIOs. Remove
any 120 ohm termination jumper on the breakout. Drive the TWAI peripheral in
listen-only mode.

The bike has at least four CAN networks (see the CAN section of
[mbb-reference.md](mbb-reference.md)). The pair on OBD pins 6 and 14 is
probably the OBD/CCM bus rather than the powertrain bus, so the sniffer may
see diagnostics and telematics traffic rather than motor and BMS frames. If
that bus follows the OBD-II convention it runs at 500 kbit/s, which is the
first rate to try.

Optional: a JST-XH 3-pin (GND/TX/RX) for bare TTL access.

## Key-switched power

Goal: no parasitic drain with the key off.

Sense: pin 8 through 100k to an RC node, 10 uF from the node to GND. The
roughly 1 s hold stops MBB chatter toggling it.

- Option A: RC node drives the buck's EN. Needs a buck with an exposed
  active-high EN.
- Option B: RC node drives a 2N7000 gate; its drain pulls a P-FET (AO3401 or
  FQP27P06) gate low; the P-FET switches 13 V to any buck. 100k pull-up on the
  P-FET gate.

The frunk USB is key-switched 5 V and would also work. Rejected so phone and
watch chargers can stay plugged in there.

## Parts

- [x] OBD-II male plug with all 16 pins and shell
- [x] 4x AITRIP ESP32 DevKit V1 USB-C (due 2026-09-12)
- [x] SH-U09B3 CP2102N (backup and interim adapter)
- [ ] Buck 13 V to 5 V, 500 mA or more, low quiescent current, EN exposed (or
      the FET switch parts)
- [ ] Inline fuse holder and 0.5 to 1 A fuse
- [ ] TVS about 24 V (SMBJ24A), optional
- [ ] SN65HVD230 breakout
- [ ] 2N7000, P-FET, 100k (option B only)
- [ ] R and C for key sense (have)
- [ ] JST-XH 3-pin (optional)
- [ ] 2x about 80 ohm 5 to 10 W resistors (only if needed after measuring)
