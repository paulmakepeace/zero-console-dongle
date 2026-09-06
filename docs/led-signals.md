# LED turn signals and the bulb-out fault

## How the fault works

Gen3 bikes have no flasher relay. The MBB switches the signals directly and
measures current per side. Front and rear on each side are one MBB channel
wired in parallel, so the MBB only ever sees the summed side current. One load
resistor per side is all that makes sense, sized with all four LEDs installed
(a remaining incandescent adds to the sum).

When the dash shows the fault, the console prints the measured current. Use
that number rather than guessing.

## Stock (incandescent) mode

The floor is near 1.3 A per side, set to notice one of two 10 W bulbs missing.
Measurements on this bike, front incandescent and rear LED on each side:

| Extra load per side | Side current   | Result |
|---------------------|----------------|--------|
| none                | 872 mA, 879 mA | fails  |
| 33 ohms             |                | fails  |
| 28 ohms             |                | passes |

A 10 W incandescent draws about 760 mA at 13.2 V, so the unloaded figure is
the front bulb plus roughly 100 mA of rear LED. The 28 ohm resistor adds about
470 mA and 6 W of heat, which is why LED mode is the better answer.

## LED mode

The console has an LED mode that needs no login. `config` lists a 14-item
install table; item 5 is "LED Indicators" (Detected: N/A, a pure flag).
`config 5` toggles it to Installed. It works on this MY2020 SR/S on firmware
revision 44, and owners report it on MY2020 and MY2023 SR/F. The table is a
bitfield with item N on bit N, so the LED flag is bit 5 (value 32). The dealer
value `accessory_configuration` 0x36 decodes to items 1, 2, 4 and 5, which
matches this bike's set, so it is likely the same field
([details](mbb-reference.md#config-table)).

**The toggle takes effect at the next key cycle, not immediately.** The MBB
writes the config word at key-off and applies it at boot. Indicating right
after `config 5` still produces the fault under the old mode, and the printed
current belongs to that mode.

Be careful with `config N`: items 8 to 14 include Charger Upgrade, Range
Upgrade and similar.

LED mode has a floor and no ceiling. The floor is around 4 to 5 W per side
(about 300 mA):

| Load per side                                         | Current | Result |
|-------------------------------------------------------|---------|--------|
| 1 W LEDs, two, no resistor (owner report)             |         | fails  |
| same plus 80 ohms (owner report)                      |         | passes |
| owner report                                          | 422 mA  | passes |
| front incandescent, rear LED, no resistor (this bike) | 872 mA  | passes |

So a mixed load clears LED mode with nothing added. The fault is set when the
indicator comes on and cleared when it goes off, and the console prints one
current line per event:

    Fault set: LEFT_BLINKER_BULB_OUT
    blinker current 872 ma

`pdu` shows the two blinker channels with live current, so a side's draw can
also be read while indicating without waiting for a fault.

## Resistor sizing

None is needed while the fronts are incandescent and LED mode is on.

Once all four signals are LEDs, two at roughly 100 mA each sit under the
300 mA floor, so expect the fault to return and about 80 ohms per side to
clear it, under 2 W, a 5 W ceramic part. Measure first: indicate with the
console open and read the printed current, or read the blinker channel in
`pdu`. The resistor goes across the rear signal leads, one per side.
