# MBB console reference

What the console offers without a login, as captured from this bike on
firmware revision 44. Identifiers (VIN, serials, CAN node identities) are left
out on purpose.

## Output format

Lines end in LF, most followed by a NUL byte. Map LF to CRLF on the way in
(`picocom --imap lfcrlf`) and strip NULs before grepping a capture
(`tr -d '\000'`).

The MBB prints unsolicited lines whenever something happens: `DEBUG:` lines
carrying a timestamp, source file and line number, plus bare state lines such
as `State change from STOP to HIB`. They interleave with whatever you are
typing. Faults arrive as `Fault set: NAME` and `Fault cleared: NAME`, and the
blinker fault is followed by `blinker current N ma`.

The console keeps answering commands after the key is turned off, at least
for a while. How long is not yet measured.

## Commands

Everything under "MBB Operational Information" is read-only and works without
a login. "MBB Settings Control" and "MBB Developer Tools" list nothing until
you log in.

| Command        | What it prints                                        |
|----------------|-------------------------------------------------------|
| `help`         | this catalogue                                        |
| `login`        | login level, or log in                                |
| `version`      | board and firmware revision                           |
| `dash version` | dash and Telit (cellular modem) firmware revision     |
| `time`         | current time                                          |
| `in`           | all measured inputs: DC-DC, 12 V rails, switches, throttle, temperatures |
| `faults`       | active and pending faults; `faults -v` lists every fault name with state |
| `notif`        | active notifications                                  |
| `stats`        | the settings table: firmware rev, counters, odometer at last DTC clear, OBD codes |
| `state`        | bike operational state                                |
| `status`       | bike status                                           |
| `pdu`          | Power Distribution Unit channels with live current    |
| `dash`         | dash information                                      |
| `msc`          | Motorcycle Stability Control info                     |
| `controller`   | controller info                                       |
| `heater`       | battery heater info                                   |
| `ccm`          | Cellular Communication Module info                    |
| `config`       | the 14-item install table; `config N` toggles item N  |
| `charging`     | charging state, schedule, and the chargers            |
| `bms`          | BMS snapshot; sub-commands interface, commands, elig, module, status, info |
| `performance`  | range filter data and calculated Wh                   |
| `obd`          | OBD-II status: MIL, DTC counts, stored and freeze-frame DTC |
| `compat`       | compatibility field and mask                          |
| `notif_pool`   | error and notification pool                           |
| `update`       | firmware update info                                  |
| `ltsm`         | long-term storage mode commands                       |
| `eldh`         | event log in hex; on rev 44 answers "log printing not supported" |
| `elddh`        | event log in hex from a date; same caveat             |

No text event-log dump exists on this revision: `eld`, `elde` and
`eventlogdump` are all rejected. Logs come from the Zero app, not the console.

## Config table

Item numbers as `config` lists them. The state column is this bike's.

| # | Item             | State         | Detected     |
|---|------------------|---------------|--------------|
| 1 | OE 3KW Charger   | Installed     | Detected     |
| 2 | ACC 3KW Charger  | Installed     | Detected     |
| 3 | ACC 6KW Charger  | Not Installed | Not Detected |
| 4 | Heated Grips     | Installed     | N/A          |
| 5 | LED Indicators   | Installed     | N/A          |
| 6 | 500W DC-DC       | Not Installed | N/A          |
| 7 | RESERVED         | Not Installed | Not Detected |
| 8 | Charger Upgrade  | Installed     | Detected     |
| 9 | Flat6kW Charger  | Not Installed | Not Detected |
| 10 | Park Mode       | Installed     | Detected     |
| 11 | Boost upgrade   | Installed     | N/A          |
| 12 | Range upgrade   | Not Installed | Not Detected |
| 13 | Ext charging    | Not Installed | Not Detected |
| 14 | 6kW upgrade     | Not Installed | Not Detected |

The table is one integer with item N on bit N. Toggling item 5 was logged at
key-off as `Config old: 3351 new: 3383 changed: 32`, and 3351 decodes to bits
1, 2, 4, 8, 10, 11 (the installed items above) plus bit 0. The dealer value
`accessory_configuration` 0x36 is bits 1, 2, 4 and 5: both chargers, heated
grips and LED indicators, which is this bike's set once item 5 is on. That
supports, without proving, the two being the same field.

The change is written to non-volatile storage at key-off and applied at the
next boot. A toggle has no effect on the running bike until the key has been
cycled, and it is lost if power is pulled before key-off.

## PDU channels

`pdu` prints one row per channel with its current in mA and a fault column.
Channels: 12V_Switch_Pwr, ABS_MCU, High_Beam, Horn, Heated_Accs, Aux_Outlet,
Headlight, Cntlr_Logic, Brake_Light, Right_Blinker, Left_Blinker, 12V_Heater,
DC-DC_to_Batt, 12V_Heater_Temp, Total_Current.

The two blinker channels are separate, which is the hardware fact behind one
resistor per side. Running `pdu` while indicating reads that side's current
without waiting for a fault, though it is a snapshot and can land on the off
half of the blink.

## Fault names

`faults -v` lists these. Blinker faults come in SHORT and BULB_OUT pairs per
side, so an over-current in LED mode is reported as BULB_OUT rather than
SHORT.

    LOGGING HIGH_THROTTLE HIGH_THROT_NOTIFY NO_VALID_TORQUE TORQUE_MAP
    THROTTLE_MAP POWERTRAIN_CAN SET_MAX_RPM INVALID_RPM CONTROLLER_WARNING
    CONTROLLER_FAULT MAP_RIDING_MODE SET_MTC_MODE MSC_NOT_ALIVE MSC_CAN
    MSC_MODE_INVALID MSC_ERROR HIGHBEAM_SHORT HIGHBEAM_BULB_OUT LOWBEAM_SHORT
    LOWBEAM_BULB_OUT LEFT_BLINKER_SHORT RIGHT_BLINKER_SHORT
    LEFT_BLINKER_BULB_OUT RIGHT_BLINKER_BULB_OUT BRAKE_SHORT BRAKE_BULB_OUT
    12V_HEATER_SHORT 12V_HEATER_DISCONNECTED 12V_BATT_REVERSED 12V_BATT_LOW
    DC_DC_LOW 12V_BATT_COLD 12V_COMBINED_LVC 12V_COMBINED_LVW 12V_DENDRITE
    PDU_AUX_SHORT PDU_HEATED_ACC_SHORT PDU_HORN_SHORT PDU_CONT_LOGIC_SHORT
    PDU_ABS_MCU_SHORT PDU_12V_SWITCHED_PWR_SHORT HVIL_OPEN TEST_LIMP SELFTEST
    MODULE_INELIGIBLE ALL_CONTACTORS_OPEN MODULE_AWAITING_CONNECTION
    MODULE_DISABLED MODULE_TOO_MANY_RETRIES LOW_SOC PILOT_SIGNAL_INVALID
    REGION_INVALID CHARGER_NOT_CONNECTED CHARGER_ERROR
    IMMOBILIZER_DISCONNECTED IMMOBILIZER_ERROR HIGH_MOTOR_TEMP HEATED_GRIP
    ISOLATION ISOLATION_WARNING STATS_WRITE STATS_STARTUP SETTINGS_WRITE
    SETTINGS_STARTUP MFG_STARTUP INCOMPATIBLE_DEVICE
    NOT_ALL_MODULE_FEATURES_SUPPORTED BMS_CELL_TOO_HIGH_FOR_CHARGE
    BMS_CELL_TOO_LOW_FOR_CHARGE BMS_RESERVE_VOLTAGE BMS_CELL_ANOMALY
    BMS_BATT_TEMP_HIGH BMS_BATT_TEMP_LOW BMS_CONTACTOR BMS_PRECHARGE
    BMS_DISCHARGE BMS_CHARGE BMS_CURRENT_SENSOR BMS_TEMP_SENSOR BMS_GENERAL
    BMS_STATE BMS_ALERT OBD_CCM_CAN CHARGER_DASH_CAN QUEUE_SEND_FAIL
    AMBIENT_TEMP_READ_FAIL THROTTLE_SWITCH_MISMATCH CCM_AUTHENTICATION
    BMS_VOLTAGE_LOW_CRITICAL MODULE_CHARGE_INELIGIBLE FW_UPDATE_FAIL
    FW_UPDATE_FAIL_CELL_SIGNAL 12V_NOT_BEING_CHARGED MOTOR_OVER_TEMP
    INVERTER_HIGH_TEMP INVERTER_OVER_TEMP PDU_OVERCURRENT
    HEATED_GRIPS_DISCONNECTED WRONG_CHARGER_BUS MODULE_ELIGIBILITY_TIMEOUT
    PARK_MODE CONTROLLER_MISMATCH SIGNIFICANT_PWR_LIMIT BMS_12V_OUT_OF_RANGE
    CHARGER_FAN KEY_OFF_WHILE_MOVING KEY_OFF_WHILE_MOVING_LIMP

## App logs versus the console

The Zero app's "Email bike logs" delivers two 131 KB files per pull, one for
the MBB and one for the BMS, named with the date, the VIN and the ECU id
(6 for the MBB, 10 for the BMS). On this firmware they are the newer compact
format that zero-log-parser documents as "2025+" and only partly decodes:
the header is not recognised, most entries come out as single characters, and
the odometer fields are unreliable.

Read raw, the MBB file is a ring of the same narration the console prints,
`Control flags changed`, `State change from STRT to PWSU`, `Requesting 12v
charge`, `Fault cleared: HVIL_OPEN`, `Blinker cancelled`, plus binary
telemetry records for vehicle state and sensors. Two things follow:

- The ring is dominated by the hourly hibernation wakes. Every hibernate
  entry reads "Hibernating for 3600 sec"; the wake reason is the RTC about
  five times out of six and the wake pin otherwise. Each wake tops up the 12 V
  battery, whose voltage sits between 12.95 and 13.02 V in the entries, and
  logs a few dozen lines. In one pull that sequence appears 55 times and
  accounts for most of the roughly 2,200 entries, so ride and fault history is
  squeezed into what is left, and a pull reaches back only about two days of
  parked time.
- Detail is dropped. The blinker fault appears, but the `blinker current` line
  that follows it on the console does not, and none of the command outputs
  (`pdu`, `in`, `bms`) exist in the log at all.

So a dongle that logs the console stream continuously holds a superset of the
MBB log for every period pin 8 is live, with the currents kept and no ring
crowding. The BMS file is the one thing the console does not replace; its
content is the module's own record and the console's `bms` view is a summary.

## CAN networks

Four are named in the fault list: POWERTRAIN_CAN, MSC_CAN, OBD_CCM_CAN and
CHARGER_DASH_CAN, and the console mentions a `CAN2txQ`. The BMS module and
the chargers join by CANopen LSS node assignment at key-on. Which of these
buses reaches OBD pins 6 and 14 is not established; the name OBD_CCM_CAN and
the `obd` DTC facility make it the likely one.
