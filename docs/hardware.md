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
it and aim it at the plastic side of the shell, not the pin block.

## How the MBB's console pins behave

This decides the wiring and the power design.

- **Pin 8, MBB TX**, is high whenever the MBB's console block is powered:
  with the key on, and in hibernation for as long as pin 9 is held high. It
  is not a key sense. With nothing driving pin 9 a sleeping MBB leaves it at
  0 V, or about 0.5 V if an adapter's input pull-up is back-feeding it.
- **Pin 9, MBB RX, is also the hibernation wake pin.** A high level on it
  resets a sleeping MBB: the boot banner prints with `Reset Source: Hib Wake
  Pin`, the MBB self-tests, goes STRT to WAIT, then STOP and HIB within about
  30 s, and stays in HIB with its console up for as long as pin 9 is high.
  About 25 s after pin 9 drops, pin 8 drops. A reset that lands during the
  hourly wake abandons the 12 V top-up.

So a dongle must never drive pin 9 while the MBB sleeps. The firmware drives
TX only while pin 8 is high, and a sleeping MBB cannot be commanded without
booting it. Whether pin 8 rises on the MBB's own hourly wake with pin 9 left
low is not yet verified; see [open-questions.md](open-questions.md).

## Wiring

Phase 1, bench and frunk USB, no added parts:

| OBD pin | Goes to                                                        |
|---------|----------------------------------------------------------------|
| 5       | GND                                                            |
| 8       | GPIO33 (D33). UART2 RX through the matrix, internal pull-down. RTC-capable, so it can wake the ESP32 from deep sleep later |
| 9       | GPIO17 (TX2). UART2 TX only while pin 8 is high; otherwise an input with the internal pull-down |
| 6       | SN65HVD230 CANH                                                |
| 14      | SN65HVD230 CANL                                                |
| 16      | phase 2 supply, below                                          |

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

## CAN

WCMCU-230 breakout, an SN65HVD230 marked VP230, 3.3 V logic. Before it goes
near the bike: remove the 120 ohm termination, R2 marked 121, because the
bike's bus is terminated at both ends already and a third 120 ohm loads the
pair to 40 ohms. The board straps RS low through 10k, so the driver is live;
for a hardware guarantee of listen-only tie CTX to 3V3 through 10k, since a
high driver input is recessive and the part then cannot assert a dominant bit
whatever the ESP32 does. CRX to GPIO4 or GPIO5, both RTC-capable in case CAN
activity becomes a wake source. RS to a GPIO is the later upgrade if transmit
is ever wanted. TWAI in listen-only mode on the ESP32 side as well.

The bike has at least four CAN networks (see the CAN section of
[mbb-reference.md](mbb-reference.md)). The pair on OBD pins 6 and 14 is
probably the OBD/CCM bus rather than the powertrain bus, so the sniffer may
see diagnostics and telematics traffic rather than motor and BMS frames. If
that bus follows the OBD-II convention it runs at 500 kbit/s, which is the
first rate to try.

## Power

Phase 1: 5 V into the DevKit's USB-C, from the frunk socket on the bike and a
powerbank on the bench. The frunk socket is key-switched, so nothing runs
while the bike sleeps.

Phase 2: always-on 13 V from pin 16, in this order: inline mini blade fuse
holder with a 1 A fuse, a master cut switch, a 1.5KE18A TVS across the rail
with its cathode to +13 V, then the buck into the DevKit's VIN, with 100 uF
25 V and 100 nF at the buck input. The buck is a 5 V module rated to 32 V in;
if it has a trim pot, set 5.0 V on the bench before it touches the DevKit and
varnish the pot. The 18 V part stands off anything the bike's charging does
and clamps near 25 V, under the buck's rating; a 24 V part clamps near 39 V,
which is not.

A key-switched supply sensed from pin 8 was designed and dropped: pin 8 does
not distinguish key on from off. Instead the dongle gates its own activity on
pin 8 and sleeps between MBB sessions. Drain: a DevKit V1 in deep sleep draws
about 10 mA through its linear regulator, about 4 mA from the 13 V side or
100 mAh a day, and an always-awake ESP32 with WiFi idling about 20 mA from
13 V. The MBB tops the 12 V battery up from the pack every hour, so either is
affordable; the master switch covers long-term storage. The 5 V rail sees
150 to 250 mA average with WiFi up and peaks near 500 mA on transmit, plus
about 20 mA for the CP2102 and the transceiver, which the frunk socket and
any 1 A buck cover.

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
