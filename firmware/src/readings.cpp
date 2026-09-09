// The readings table: a whitelist of row names, each with its group and
// unit pinned here rather than read from the row, since the third column
// of a console table is a unit in some outputs and a label in others.
// Order is display order.
#include "readings.h"
#include "clock.h"
#include "pure/rows.h"

struct Def { const char* name; const char* group; const char* unit; };
static const Def DEFS[] = {
    {"lowest_cell_voltage_mv", "pack", "mV"}, {"pack_capacity_ah", "pack", "Ah"},
    {"min_pack_temp_c", "pack", "C"}, {"max_pack_temp_c", "pack", "C"},
    {"pack_current_ma", "pack", "mA"}, {"max_charge_voltage", "pack", "V"}, {"target_charge_voltage", "pack", "V"},
    {"max_charge_current", "pack", "A"}, {"max_discharge_current", "pack", "A"},
    {"Motor_RPM", "motor", "rpm"}, {"Actual_Torque", "motor", "uNm"}, {"Req_Torque", "motor", "uNm"},
    {"Motor_Temp", "motor", "C"}, {"Inverter_Temp", "motor", "C"},
    {"DC_Bus_Voltage", "motor", "mV"}, {"DC_Bus_Current", "motor", "mA"},
    {"Lean", "attitude", "deg x10"}, {"Pitch", "attitude", "deg x10"}, {"Tip_Angle", "attitude", "deg x10"},
    {"Yaw_rate", "attitude", "mdeg/s"}, {"Front_Wheel_Speed", "attitude", "mm/h"}, {"Rear_Wheel_Speed", "attitude", "mm/h"},
    {"ABS_Mode", "attitude", ""}, {"MTC_Mode", "attitude", ""}, {"Bike_Tipped_Over", "attitude", ""},
    {"Speed_kph", "trip", "km/h"}, {"State_of_Charge", "trip", "%"}, {"Estimated_Range_km", "trip", "km"},
    {"Odometer_km", "trip", "km"}, {"Odometer_mi", "trip", "mi"}, {"Active_Ride_Mode", "trip", ""}, {"total_Whr", "trip", "Wh"},
    {"DC-DC", "12v", "mV"}, {"12V_Battery", "12v", "mV"}, {"12V_Combined", "12v", "mV"}, {"BMS_12V", "12v", "mV"},
    {"cell_signal_percent", "cell", "%"}, {"cell_network_registration", "cell", ""}, {"connected_to_starcom", "cell", ""},
    {"gps_is_valid", "cell", ""}, {"storage_mode", "cell", ""}, {"hb_soc", "cell", "%"},
    {"EVSE_Connector_State", "charge", ""}, {"Pilot_Current", "charge", "A"}, {"Chargers_Connected", "charge", ""},
    {"Active_DTCs", "faults", ""}, {"Pending_DTCs", "faults", ""}, {"MIL_On", "faults", ""},
};
static const int N = sizeof DEFS / sizeof DEFS[0];

struct Slot { long value; uint8_t decimals; uint32_t atMs; long epoch; };   // atMs 0 and epoch 0: never seen
static Slot slots[N];

static int find(const RowValue& r) {
    for (int i = 0; i < N; i++) if (rowNameIs(r, DEFS[i].name)) return i;
    return -1;
}

static void note(const char* line, size_t len, long epoch) {
    RowValue r;
    if (!parseRow(line, len, r) || !r.valid) return;   // an invalid figure leaves the last good one
    int i = find(r);
    if (i < 0) return;
    slots[i].value = r.value;
    slots[i].decimals = r.decimals;
    if (epoch) { slots[i].atMs = 0; slots[i].epoch = epoch; }
    else { slots[i].atMs = millis() ? millis() : 1; slots[i].epoch = 0; }
}

void readingsNoteLine(const char* line, size_t len) { note(line, len, 0); }

void readingsFeed(const char* text, size_t len, long epoch) {
    if (!epoch) return;   // a saved output with no wall time has no age to show
    size_t s = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > s) note(text + s, i - s, epoch);
            s = i + 1;
        }
    }
}

static long ageS(const Slot& x) {   // as the poller counts it: -1 never, -2 from before this boot while the clock is unset
    if (x.atMs) return (long)((millis() - x.atMs) / 1000);
    if (x.epoch) return clockValid() ? (long)time(nullptr) - x.epoch : -2;
    return -1;
}

String readingsJson() {
    String s;
    s.reserve(80 * N);
    s += "[";
    bool first = true;
    for (int i = 0; i < N; i++) {
        const Slot& x = slots[i];
        if (!x.atMs && !x.epoch) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"n\":\"" + String(DEFS[i].name) + "\",\"g\":\"" + DEFS[i].group + "\",\"v\":";
        long scale = 1;
        for (uint8_t d = 0; d < x.decimals; d++) scale *= 10;
        long whole = x.value / scale, frac = x.value % scale;
        if (x.value < 0 && whole == 0) s += "-";
        s += String(whole);
        if (x.decimals) {
            char f[8];
            snprintf(f, sizeof f, ".%0*ld", (int)x.decimals, frac < 0 ? -frac : frac);
            s += f;
        }
        s += ",\"u\":\"" + String(DEFS[i].unit) + "\",\"age_s\":" + String(ageS(x)) + "}";
    }
    return s + "]";
}
