// The sleep policy owns one decision: when the dongle may stop. It sleeps
// only with the MBB asleep, nobody using the dongle, and a grace period
// gone by, for as long as the MBB's own timer allows minus a lead, and it
// hands the network down and up around the sleep.
#include "sleep.h"
#include "config.h"
#include "clock.h"
#include "store.h"
#include "net.h"
#include "pure/hibernate.h"
#include <Preferences.h>
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"

static bool enabled = false;
static uint32_t afterDays = 3;
static long lastAttendedS = 0;    // wall time the bike was last seen attended; 0 for never seen
static bool attendedDirty = false;   // needs saving, done while the MBB sleeps
static bool haveHib = false;
static long hibAtS = 0;          // seconds on the clock the store stamps with; wall time once NTP or the MBB set it
static long hibSec = 0;
static bool intermediate = false;   // the last wake was a chunk boundary, not the MBB's due time
static long sleptUncorrectedS = 0;  // sleep measured by the RC clock and not yet corrected by NTP
static uint32_t lastWakeMs = 0;
static bool wasAwake = false;
static uint32_t asleepSinceMs = 0;
static uint32_t sleeps = 0;
static uint32_t lastSleptS = 0;
static const char* lastWake = "none";
static long plannedS = 0;

void sleepBegin(bool on, uint32_t days, long lastS) {
    enabled = on;
    afterDays = days;
    lastAttendedS = lastS;
    asleepSinceMs = millis();
    // Pads keep their running configuration through light sleep: the
    // transmit pin and pin 8 stay pulled-down inputs, so the MBB's wake pin
    // cannot drift high and the wake on pin 8 works.
    gpio_sleep_sel_dis((gpio_num_t)PIN_MBB_TX);
    gpio_sleep_sel_dis((gpio_num_t)PIN_MBB_RX);
}
void sleepSetEnabled(bool on) { enabled = on; }
bool sleepEnabled() { return enabled; }
void sleepSetAfterDays(uint32_t d) { afterDays = d; }
uint32_t sleepAfterDays() { return afterDays; }

static long nowS() { return (long)time(nullptr); }   // advanced across light sleep by the RTC, put right by NTP after each wake

void sleepNoteLine(const char* line, size_t len) {
    long s = parseHibernateSeconds(line, len);
    if (s > 0) { haveHib = true; hibAtS = nowS(); hibSec = s; sleptUncorrectedS = 0; }
    if (isBikeAttended(line, len) && clockValid()) { lastAttendedS = nowS(); attendedDirty = true; }
}

// The bike counts as unattended once N days have passed since a top-up or a
// key-on. With no such event ever seen, the count runs from the clock's first
// fix, so a fresh board does not sleep on day one.
static bool unattended() {
    if (afterDays == 0) return true;
    if (!clockValid()) return false;
    long since = lastAttendedS ? nowS() - lastAttendedS : 0;
    return since > (long)afterDays * 86400L;
}

static void doSleep(long seconds, long untilWakeS) {
    Serial.printf("sleep: %ld s of the %ld s until the MBB is due\n", seconds, untilWakeS);
    Serial.flush();
    storeTick(true);   // whatever is pending, while it is quiet
    netSuspend();
    sysFeedWatchdog();
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    gpio_wakeup_enable((gpio_num_t)PIN_MBB_RX, GPIO_INTR_HIGH_LEVEL);   // pin 8 rising: the MBB is up
    esp_sleep_enable_gpio_wakeup();
    uint32_t before = millis();
    esp_light_sleep_start();
    sysFeedWatchdog();
    gpio_wakeup_disable((gpio_num_t)PIN_MBB_RX);
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    lastWake = cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" : cause == ESP_SLEEP_WAKEUP_GPIO ? "pin 8" : "other";
    lastSleptS = (millis() - before) / 1000;
    sleptUncorrectedS += seconds;   // what the RC clock claims; NTP may correct it before the next plan
    lastWakeMs = millis();
    sleeps++;
    Serial.printf("sleep: woke on %s after %lu s\n", lastWake, (unsigned long)lastSleptS);
    netResume();
    String note = clockStamp() + " dongle: slept " + String(lastSleptS) + " s, woke on " + lastWake;
    storeAppend(note, false);
    intermediate = cause == ESP_SLEEP_WAKEUP_TIMER && seconds < untilWakeS - SLEEP_LEAD_S;
    asleepSinceMs = millis();   // a fresh grace period: short for a chunk boundary, full when the MBB is due
}

void sleepTick(bool mbbAwake, bool busy) {
    uint32_t now = millis();
    if (mbbAwake != wasAwake) {
        wasAwake = mbbAwake;
        if (!mbbAwake) asleepSinceMs = now;
    }
    if (attendedDirty && !mbbAwake) {   // a flash write, so only while the MBB sleeps
        attendedDirty = false;
        Preferences p;
        if (p.begin("dongle", false)) { p.putLong("attended", lastAttendedS); p.end(); }
    }
    if (!enabled || mbbAwake || busy) return;
    if (!unattended()) return;
    if (now - asleepSinceMs < (intermediate ? SLEEP_REGRACE_MS : SLEEP_GRACE_MS)) return;
    if (sleptUncorrectedS && clockNtpAgeS() * 1000UL < now - lastWakeMs) sleptUncorrectedS = 0;   // NTP has put the clock right since the wake
    long until = secondsUntilMbbWake(haveHib, hibAtS, hibSec, nowS(), sleptUncorrectedS, SLEEP_DRIFT_PCT, SLEEP_FALLBACK_S);
    plannedS = sleepChunk(until, SLEEP_LEAD_S, SLEEP_CHUNK_S, SLEEP_DRIFT_PCT, SLEEP_MIN_S);
    if (plannedS == 0) {
        // The MBB's wake is due or overdue. Stay up for it; if it never comes,
        // the announcement is stale and the fallback timer takes over.
        if (haveHib && until < -60) haveHib = false;
        intermediate = false;
        return;
    }
    doSleep(plannedS, until);
}

String sleepStatusJson() {
    long due = haveHib ? secondsUntilMbbWake(true, hibAtS, hibSec, nowS(), sleptUncorrectedS, SLEEP_DRIFT_PCT, 0) : -1;
    long since = lastAttendedS && clockValid() ? nowS() - lastAttendedS : -1;
    return String("{\"enabled\":") + (enabled ? "true" : "false") + ",\"after_days\":" + String(afterDays) +
           ",\"attended_age_s\":" + String(since) + ",\"armed\":" + (enabled && unattended() ? "true" : "false") +
           ",\"count\":" + String(sleeps) +
           ",\"last_wake\":\"" + lastWake + "\",\"last_slept_s\":" + String(lastSleptS) +
           ",\"mbb_wake_due_s\":" + String(due) + "}";
}
