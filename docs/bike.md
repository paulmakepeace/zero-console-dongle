# This bike

Zero SR/S, MY2020. Identity numbers (VIN, MBB serial, battery and
charger serials) stay out of this repo; they are in the console captures under
`logs/`, which git ignores.

## MBB

| Field         | Value                                  |
|---------------|----------------------------------------|
| Board         | MBB PDU POTTED GEN 3 STEP D, PN 40-08198, board ID 04 |
| Firmware      | FIRMWARE MBB GEN3, PN 75-08163, revision 44 |
| Build         | 2025-12-17, bank A                     |
| Delivered by  | over-the-air through the CCM; the stats table records the update as succeeded |
| Dash          | major version 8                        |

## Fitted options

From the `config` table: both 3 kW chargers (OE and accessory, with the
Charger Upgrade flag), heated grips, Park Mode, Boost upgrade, and LED
indicators. One battery module.

## Snapshot 2026-09-05

Point-in-time readings, kept so a later session can see what moved.

| Reading                     | Value                    |
|-----------------------------|--------------------------|
| Key cycles                  | 1958                     |
| Total on time               | 11,336,152 s (about 131 days) |
| Total run time              | 860,023 s (about 239 h)  |
| Total charger time          | 2,672,703 s (about 742 h) |
| Max pack temperature        | 46 C                     |
| Max motor temperature       | 37 C                     |
| Odometer at last DTC clear  | 12,729 km                |
| Pack voltage, SOC           | 107.3 V, 72 %            |
| Charge target               | 85 %                     |
| DC-DC output                | 13.01 V                  |
| 12 V battery                | 12.90 V                  |
| Ambient                     | 30.9 C                   |
| Total 12 V load (key on, grips on) | 4.94 A            |

Stored OBD state: MIL off, one active DTC `B1A0F`, first seen at key cycle
1471 with the freeze frame kept. The freeze frame's odometer field matches the
odometer at the time of the capture (about 9000 mi, 14,480 km), so the code
was refreshed in this session, and the blinker bulb-out fault is the likely
subject. Not confirmed.

Faults seen at key-on in this session, all transient: CONTROLLER_WARNING
(code 200) for three seconds, and HIGH_THROTTLE twice. The blinker faults are
covered in [led-signals.md](led-signals.md).
