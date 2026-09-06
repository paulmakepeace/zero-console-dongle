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

Pin 8 stays live for at least 20 s after key-off, because the MBB keeps its
console up while it shuts down, and reads 0 V once the MBB has gone to sleep.
So the dongle gets a shutdown window of that length rather than an instant
cut, and can log the shutdown sequence. The full duration is not yet measured.

The MBB also wakes itself every hour: every hibernate entry in the app logs
says "Hibernating for 3600 sec", and each wake tops up the 12 V battery from
the pack, prints a few dozen lines, and sleeps again. A pin-8-sensed dongle
will therefore power up hourly for the length of that cycle, which is not yet
measured either. It is a feature for logging and a cost of a few tens of
milliamp-seconds against a battery the wake is charging anyway.

Switch: the RC node drives a 2N7000 gate; its drain pulls a P-channel MOSFET's
gate low through the 100k gate pull-up to +13 V; the P-FET switches the fused
13 V into the buck. Key on: node high, 2N7000 on, gate low, P-FET on. Key off:
node decays, 2N7000 off, gate rises to 13 V, P-FET off, and the only drain is
FET leakage. A 1M bleed from the RC node to GND guarantees the decay if pin 8
goes high-impedance rather than low.

The P-FET must tolerate the full 13 V gate swing: pick one rated for at least
20 V gate-to-source. The AO3401 is rated 12 V and needs a 100k/100k divider or
a 10 V zener on its gate if used. Driving a buck's own enable pin instead does
not work with common modules, whose enable is pulled up internally and needs
pulling low to switch off, the opposite polarity to the key sense.

The frunk USB is key-switched 5 V and would also work. Rejected so phone and
watch chargers can stay plugged in there.

## Parts

Power budget on the 5 V rail: the ESP32 averages 150 to 250 mA with WiFi up
and peaks near 500 mA on transmit, the onboard CP2102 and the CAN transceiver
add about 20 mA between them. Size the buck for 1 A or more so the peaks do not
brown it out; low quiescent current does not matter because the P-FET removes
the whole circuit at key-off.

- [x] OBD-II male plug with all 16 pins and shell
- [x] 4x AITRIP ESP32 DevKit V1 USB-C (due 2026-09-12)
- [x] SH-U09B3 CP2102N (backup and interim adapter)
- [x] 2x 80 ohm ceramic power resistors (for the all-LED phase if the fault
      returns)
- [ ] Buck, 5 V out, 1 A or more, input rated 30 V or better. Pololu D24V22F5
      class, or an MP1584EN module set to 5.0 V on the bench before it goes
      near the DevKit
- [ ] P-channel MOSFET, 30 V or more drain-source, 20 V or more gate-source,
      logic level: FQP27P06 or IRF9540N through-hole, DMG2305UX or SI2319 SMD
- [ ] 2N7000
- [ ] Resistors: 100k x2 (gate pull-up, key-sense series), 1M (bleed), 1k x2
      (series in the UART lines as cheap insurance)
- [ ] 10 uF for the key sense (have); 100 uF 25 V electrolytic and 100 nF at
      the buck input
- [ ] Inline fuse holder, mini blade or 5x20, with a 1 A fuse, first thing
      after pin 16
- [ ] TVS diode SMBJ24A or 1.5KE24A across the fused 13 V, before the P-FET
- [ ] SS34 Schottky in series for reverse polarity, optional; the connector is
      keyed and the diode costs 0.3 V
- [ ] SN65HVD230 breakout, 3.3 V supply, RS strapped to GND; desolder the
      120 ohm termination the breakouts ship with
- [ ] JST-XH 2.5 mm 3-pin header and housing for bare TTL access, optional
- [ ] Perfboard offcut about 20 x 30 mm for the power switch parts, 24 AWG
      silicone wire, heat shrink, Kapton, an M3 nylon washer for the post mount
