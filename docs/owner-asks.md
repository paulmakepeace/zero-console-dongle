# What owners asked to see

A 2021 thread in the Zero Motorcycles Owners Group asked what data owners
would want from a console and CAN dongle
(facebook.com/groups/111611975574686/posts/4211101315625711). The asks,
against what the MBB console offers and what the dongle shows today. Command
outputs are in [mbb-reference.md](mbb-reference.md).

| Ask | Console source | On the dongle now |
|-----|----------------|-------------------|
| State of health, the most repeated ask, "priceless for a used bike" | No SOH figure on any command. `bms interface` gives `pack_capacity_ah` against the pack's nominal, and `stats` the reset and run-time counters; a health figure would be derived | pack Ah in the status JSON |
| Battery temperature | `bms interface` min and max pack temperature; the BMS row of `status` | pack high and low temperatures in the status JSON |
| Cell average voltage and imbalance | `bms interface` lowest cell voltage and pack voltage; no per-cell list, so imbalance is the gap between the lowest cell and the average | not shown |
| Lean angle, pitch, telemetry to match with a phone's GPS | `msc`: pitch, lean, tip angle, yaw rate, three-axis acceleration, wheel speeds, ABS events; `ccm` carries the bike's own GPS fix | not shown |
| Torque requested, RPM, motor and controller temperature, DC bus voltage and current | `controller`, one row each, with a validity flag | not shown |
| Watts, time and miles | `performance` (Wh per km, total Wh), `dash info` (odometer in km and miles, speed, estimated range, ride mode), `stats` (total on and run time) | not shown |
| Advertised EVSE current and power while charging, per-charger power | `charging`: pilot current, each charger's voltage, current and state | polled every minute, on the command page |
| Fault codes and configuration | `faults`, `obd` (DTCs, freeze frame), `config` (the install table) | `faults` polled; `obd` and `config` not |
| State of charge, bike state | `status`, `bms`, `dash info` | in the status JSON, polled every minute |
| Stop a charge at a chosen SOC, Home Assistant integration, a charger controller fed by pack SOC and temperature | Read-only console; SOC and temperature are there to feed one | the status JSON is the feed; no control |
| Point-and-tap on a phone rather than typed commands; colourised output | The dongle's pages | the command page polls on tab selection; no colour |
| Which bikes, at what price | The thread's author: every Zero with the serial console, at model-dependent baud rates, plus CAN | this dongle is built for the SR/S MY2020 console |

The pattern: the most-wanted figures live in `bms interface`, `controller`,
`msc` and `dash info`, none of which the poller runs. State of health has no
source line and would have to be derived from capacity over time, which the
log makes possible.
