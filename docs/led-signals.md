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
`config 5` toggles it to Installed and it persists across a key cycle. Owners
have confirmed it on MY2020 and MY2023 SR/F. It is likely the same bitfield
dealers set as `accessory_configuration` 0x36.

Be careful with `config N`: items 8 to 14 include Charger Upgrade, Range
Upgrade and similar.

LED mode still has a threshold, around 4 to 5 W per side (about 300 mA):

| Report                                | Result |
|---------------------------------------|--------|
| 1 W LEDs, two per side, no resistor   | fails  |
| same plus 80 ohms per side            | passes |
| 422 mA per side                       | passes |

## If a resistor is still needed

About 80 ohms, 5 W ceramic or 10 W aluminium-cased, one per side, across the
rear signal leads. The rear is easy to reach and needs no fairing work.
