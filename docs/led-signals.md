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

Testing on this bike: a parallel load of 28 ohms or less per side passes, about
7 W dissipated in the resistor alone. 33 ohms fails.

## LED mode

The console has an LED mode that needs no login. `config` lists a 14-item
install table; item 5 is "LED Indicators" (Detected: N/A, a pure flag).
`config 5` toggles it to Installed and it persists across a key cycle. It works
on this MY2020 SR/S and owners report it on MY2020 and MY2023 SR/F. It is
likely the same bitfield dealers set as `accessory_configuration` 0x36.

Be careful with `config N`: items 8 to 14 include Charger Upgrade, Range
Upgrade and similar.

LED mode accepts a band of current, not just a minimum. The floor is around
4 to 5 W per side (about 300 mA); the ceiling lies somewhere between 422 mA
and 872 mA:

| Load per side                                   | Mode | Current | Result |
|-------------------------------------------------|------|---------|--------|
| 1 W LEDs, two, no resistor (owner report)       | LED  |         | fails  |
| same plus 80 ohms (owner report)                | LED  |         | passes |
| owner report                                    | LED  | 422 mA  | passes |
| front incandescent, rear LED, no resistor (this bike) | LED | 872 mA left, 879 mA right | fails |

The fault is set when the indicator comes on and cleared when it goes off, and
the console prints one current line per event:

    Fault set: LEFT_BLINKER_BULB_OUT
    blinker current 872 ma

A 10 W incandescent draws about 760 mA at 13.2 V, so the mixed-load figure is
the front bulb plus roughly 100 mA of rear LED. LED mode therefore only works
once all four signals are LEDs; with an incandescent still on the channel the
bike belongs in stock mode.

## Resistor sizing

Two phases, because the front signals need fairing work and the rear do not.

Rear LED, front incandescent, stock mode: about 28 ohms per side (see above),
which dissipates about 6 W and needs a 10 W aluminium-cased part on a bracket.
The alternative is to live with the fault until the fronts are done.

All four LED, LED mode: two LEDs at roughly 100 mA each sit under the 300 mA
floor, so expect about 80 ohms per side, under 2 W, 5 W ceramic. Measure first:
indicate with the console open and read the printed current.

Either way the resistor goes across the rear signal leads, one per side.
