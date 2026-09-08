# Dongle hardware

## Shell

OBD-II male plug shell, about 72 x 46 mm outside. Four posts about 35 x 30 mm
centre to centre, about 6 mm diameter, with the case screws running through
them. About 28 x 24 mm clear between posts. Cable-exit notch at the rear.

The DevKit's corner hole sits over one post and one other post has to come
out, leaving two case screws, which is enough for a shell that lives in a
closed compartment.

## Board

AITRIP 30-pin ESP32 DevKit V1 (WROOM-32, onboard CP2102, USB-C), about
52 x 28 mm. Remove the header pins: they bottom out on the shell floor and
cost 8 mm of height. Wires go to the pads.

The antenna is the black overhang at the module end of the board, a meandered
copper track under the matte finish. Keep wire, perfboard and posts away from
it and aim it at the plastic side of the shell, not the pin block. Under the
bike's cover in the drive the link to the house runs at -81 to -88 dBm with
a few disconnects an hour, which is the edge of what the printed antenna
can do; a WROOM-32U module with a u.FL socket and a small external antenna
is the phase 2 board option if that matters.

## How the MBB's console pins behave

This decides the wiring and the power design.

- **Pin 8, MBB TX**, is high whenever the MBB's console block is powered:
  with the key on, and in hibernation for as long as pin 9 is held high. It
  is not a key sense. With nothing driving pin 9 a sleeping MBB leaves it at
  0 V, or about 0.5 V if an adapter's input pull-up is back-feeding it.
- **Pin 9, MBB RX, is also the hibernation wake pin.** A high level on it
  resets a sleeping MBB and then holds it in a shallow hibernation with its
  console up and pin 8 high for as long as pin 9 stays high; the sequence
  and the timings are in the sleep and wake section of
  [mbb-reference.md](mbb-reference.md).

So a dongle must never drive pin 9 while the MBB sleeps, and must not hold
it high while the MBB is awake either, because a UART idles high and that
level is what keeps the MBB in its shallow hibernation after key-off. A
sleeping MBB cannot be commanded without booting it. The rule the firmware
follows is the transmit-pin rule in the design rules of
[firmware.md](firmware.md). Pin 8 rises on the MBB's own hourly wake with
pin 9 left low, from the first byte of the boot banner, so a dongle sleeping
on pin 8 sees every wake.

## Wiring

| OBD pin | Goes to                                                        |
|---------|----------------------------------------------------------------|
| 5       | GND                                                            |
| 8       | GPIO33 (D33). UART2 RX through the matrix, internal pull-down. RTC-capable, so it can wake the ESP32 from light sleep later |
| 9       | GPIO17 (TX2). UART2 TX under the transmit-pin rule; otherwise an input with the internal pull-down |
| 6       | SN65HVD230 CANH, phase 2                                       |
| 14      | SN65HVD230 CANL, phase 2                                       |
| 16      | the phase 2 supply, below                                      |

Phase 1 is pins 5, 8 and 9 with no added parts, powered over USB-C.

Phase 2 adds, at the plug end:

- 1k in series with each UART line. Current limiting for a swapped pair, a
  back-fed rail, or a transient; nothing to do with logic levels.
- 100k from pin 8 to pin 5, so a dead MBB output reads as a clean low.
- 10k from pin 9 to pin 5, so the wake pin rests low with the dongle
  unpowered or booting. Not 100k: the ESP32 enables 45k internal pull-ups on
  many pins during its ROM boot, and against 100k that puts 2.3 V on the wake
  pin, against 10k 0.6 V.

The internal pulls the firmware enables cover the running case only. External
parts are unconditional. A 3-pin connector on the plug's pigtail lets the
CP2102N adapter and the DevKit swap in, and the pull-downs suit both.

The ESP32's internal RC slow clock runs about 5% long, which the firmware
covers by sleeping for nine tenths of the wait and waking a few minutes
early; that costs milliamp-hours, not code. The 32.768 kHz crystal the
module's pins allow for is not the answer either way: no DevKit-class
board carries one, selecting it as the slow clock is a build-time option
the precompiled core does not set, and its pins are GPIO32 and GPIO33,
the latter being pin 8's input. A board that carries the crystal would
need pin 8 moved and a full core rebuild to use it, for a timing the
margin already covers. A DS3231 real-time clock module is not on the
board either: what it would buy is the time through a power cut, which
NTP gives back seconds after the join and the MBB's stamps within a
session, and a thermometer, which the BMS supplies for the pack and the
die sensor for the board.

## CAN

WCMCU-230 breakout, an SN65HVD230 marked VP230, 3.3 V logic. Before it goes
near the bike: remove the 120 ohm termination, R2 marked 121, because the
bike's bus is terminated at both ends already and a third 120 ohm loads the
pair to 40 ohms. The board straps RS low through 10k, so the driver is live;
for a hardware guarantee of listen-only tie CTX to 3V3 through 10k, since a
high driver input is recessive and the part then cannot assert a dominant bit
whatever the ESP32 does. CRX to GPIO4, RTC-capable in case CAN activity
becomes a wake source. RS to a GPIO is the later upgrade if transmit is ever
wanted. TWAI in listen-only mode on the ESP32 side as well.

Bench test before the bike: termination removed, driver input tied
recessive, listen-only at 500 kbit/s, then 250k and 125k. That identifies
which of the bike's CAN networks (listed in
[mbb-reference.md](mbb-reference.md)) is on OBD pins 6 and 14, which is an
open question; the name OBD_CCM_CAN makes it the likely one, so the sniffer
may see diagnostics and telematics traffic rather than motor and BMS frames.

## Power

Phase 1: 5 V into the DevKit's USB-C, from the frunk socket on the bike or
a wall supply. The frunk socket is key-switched: it comes up about 11 s
after key-on and dies at key-off, so a capture across the bike's sleep needs
the wall supply. Powerbanks are no good: at the dongle's draw they decide
nothing is connected and switch off, and one that pulses its output to check
resets the dongle.

Phase 2: always-on 13 V from pin 16, in this order: inline mini blade fuse
holder with a 1 A fuse, a master cut switch, a 1.5KE18A TVS across the rail
with its cathode to +13 V, then the buck into the DevKit's VIN, with 100 uF
25 V and 100 nF at the buck input. The buck is a 5 V module rated to 32 V in;
if it has a trim pot, set 5.0 V on the bench before it touches the DevKit and
varnish the pot. The 18 V part stands off anything the bike's charging does
and clamps near 25 V, under the buck's rating; a 24 V part clamps near 39 V,
which is not.

Pin 8 does not distinguish key on from off, so the supply is not
key-switched; the dongle gates its own activity on pin 8 and, once the
bike has gone days without a top-up or a key-on, sleeps between MBB
sessions. With the key on the DC-DC converter feeds the 12 V rail from the
pack and the 12 V battery with it, so the parked case is the only one the
dongle's draw matters in. Drain: the DevKit's linear regulator and USB bridge take about
5 mA whatever the ESP32 does, so a DevKit V1 in light sleep draws about
10 mA at 5 V, about 4 mA from the 13 V side or 100 mAh a day, and an
always-awake ESP32 with WiFi idling about 20 mA from 13 V. The MBB charges
the 12 V battery from the pack on a minority of its hourly wakes (see
[mbb-reference.md](mbb-reference.md)), so either is affordable; the master
switch covers long-term storage. The 5 V rail sees about 50 mA average with
WiFi associated, 0.25 W at the socket, with transmit peaks of a few hundred
milliamps, plus about 20 mA for the CP2102 and the transceiver, which the
frunk socket and any 1 A buck cover.

## Parts

Have:

- [x] 2x OBD-II male plug with all 16 pins and shell; one wired for the
      CP2102N adapter, one for the dongle
- [x] 4x AITRIP ESP32 DevKit V1 USB-C
- [x] SH-U09B3 CP2102N adapter
- [x] 2x 80 ohm ceramic power resistors, for the all-LED phase if the fault
      returns
- [x] Resistors: 1k x2, 10k x2, 100k, and 10 uF

Ordered:

- [x] WCMCU-230 SN65HVD230 breakout
- [x] Buck, 5 V out, 32 V in

To get:

- [ ] 1.5KE18A TVS, or SMBJ18A if SMD
- [ ] Inline mini blade fuse holder, 16 AWG leads, and a 1 A mini blade fuse
      such as Littelfuse 0297001.WXNV
- [ ] Mini SPST toggle or slide switch, 3 A, as the master cut
- [ ] 100 uF 25 V electrolytic and 100 nF ceramic
- [ ] SS34 Schottky in series for reverse polarity, optional; the connector
      is keyed and a unidirectional TVS already shorts a reversed supply
      through the fuse
- [ ] JST-XH 2.5 mm 3-pin header and housing for the plug pigtail
- [ ] Perfboard offcut, 24 AWG silicone wire, heat shrink, Kapton, an M3
      nylon washer for the post mount
