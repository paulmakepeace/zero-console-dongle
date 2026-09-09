# What owners asked to see

Two threads in the Zero Motorcycles Owners Group: a 2021 one asking what
data owners would want from a console and CAN dongle
(facebook.com/groups/111611975574686/posts/4211101315625711), and a 2026 one
around an owner's own phone app reading Zero's Starcom cloud with the OEM app
login (facebook.com/photo/?fbid=10165269412658619), which drew replies for
what the OEM app hides. Each ask below is mapped to the console row that
carries it, and to where the dongle shows it. Command outputs are in
[mbb-reference.md](mbb-reference.md); the readings mechanism is item 7 of
[firmware.md](firmware.md).

| Ask | Console row | On the dongle |
|-----|-------------|---------------|
| State of health, the most repeated ask | `bms interface` `pack_capacity_ah`, the same figure as the Capacity column of `status`. It read 85 Ah at 48 % and 84 Ah at 86 %, so it is a register, not remaining charge. Against a nominal 129 Ah for this pack that would be two thirds, which does not fit a five-year-old bike; what the register measures is an open question | the Ah figure, as a reading; no percentage until the register is understood |
| Battery temperature | `bms interface` `min_pack_temp_c`, `max_pack_temp_c`; the `status` pack row | readings, and the pack row in the status JSON |
| Lowest cell, cell imbalance | `bms interface` `lowest_cell_voltage_mv`; no per-cell list, so imbalance is the gap to the pack average | lowest cell as a reading |
| Lean, pitch, telemetry | `msc` `Lean`, `Pitch`, `Tip_Angle` in tenths of a degree, `Yaw_rate`, wheel speeds, ABS events | readings |
| Torque, RPM, motor and controller temperature, DC bus | `controller` `Motor_RPM`, `Actual_Torque`, `Req_Torque`, `Motor_Temp`, `Inverter_Temp`, `DC_Bus_Voltage`, `DC_Bus_Current` | readings |
| Watts, time and miles | `dash info` `Odometer_km`, `Odometer_mi`, `Estimated_Range_km`, `Speed_kph`; `performance` `total_Whr`; `stats` on and run time | readings from `dash info` and `performance`; `stats` is a one-shot |
| 12 V battery voltage and health | `in` `12V_Battery`, `DC-DC`, `12V_Combined`, `BMS_12V` in mV | readings; no health figure, the OEM's is not explained anywhere |
| EVSE pilot and per-charger power | `charging` `Pilot_Current`, `EVSE_Connector_State`, `Chargers_Connected`, the chargers table | readings for the three; the table on the command page |
| Fault codes, DTCs | `faults`; `obd` `Active_DTCs`, `MIL_On`, `Freeze_frame_DTC` | `faults` on the command page; the `obd` counts as readings |
| Cell signal, satellites, GPS fix | `ccm` `cell_signal_percent`, `cell_network_registration`, `connected_to_starcom`, `gps_is_valid`; the fix itself is in the same output | readings for the signal and the fix's validity; the coordinates are not shown and never leave the log |
| Altitude | not on the console; the CCM reports it to the cloud only | no |
| Theft attempt, anti-theft | not on the console | no |
| State of charge, bike state, charging | `status`, `bms`, `dash info` `State_of_Charge`, `ccm` `hb_soc` | status JSON and readings |
| Configuration | `config`, the install table | one-shot; the table is in mbb-reference.md |
| Stop a charge at a chosen SOC, Home Assistant | read-only console; the status JSON and readings are the feed | no control |
| Point-and-tap on a phone, colour | the dongle's pages | the command page polls on tab selection; no colour |

The commands that carry the wanted figures are `status`, `charging`, `bms`,
`bms interface`, `controller`, `msc`, `dash info`, `in`, `ccm`, `faults`,
`obd` and `performance`; the poller runs them all.
