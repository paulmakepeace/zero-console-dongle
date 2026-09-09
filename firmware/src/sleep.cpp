// The sleep policy owns one decision: when the dongle may stop. It sleeps
// only with the bike unattended, the MBB asleep, nobody using the dongle
// and a grace period gone by, once per MBB announcement for nine tenths
// of the wait, and it hands the network down and up around the sleep.
#include "sleep.h"
#include "config.h"
#include "clock.h"
#include "store.h"
#include "wlan.h"
#include "sys.h"
#include "mbb_uart.h"
#include "pure/hibernate.h"
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

static bool enabled = false;
static uint32_t afterDays = 3;
static uint32_t graceMs = SLEEP_GRACE_MS;
static long lastAttendedS = 0;       // wall time the bike was last seen attended; 0 for never seen
static bool provoked = false;        // this session was started by the dongle's own wake: its lines are not attendance
static bool attendedDirty = false;   // needs saving, done while the MBB sleeps
static int storage = 0;              // long-term storage mode as the MBB last stated it: 1 on, -1 off, 0 not known
static bool storageNote = false;     // a change to put in the log from the loop's next pass, after the MBB's own line
static SleepPlan plan;               // the MBB's announcement and whether it has been slept for
static bool wasAwake = false;
static uint32_t asleepSinceMs = 0;
static uint32_t sleeps = 0;
static uint32_t lastSleptS = 0;
static const char* lastWake = "none";

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
void sleepSetGraceMs(uint32_t g) { graceMs = g; }
uint32_t sleepGraceMs() { return graceMs; }
uint32_t sleepAfterDays() { return afterDays; }

// Advanced across light sleep by the same count the sleep timer ran on, so
// it carries that clock's error until NTP or the MBB's stamps put it right.
static long nowS() { return (long)time(nullptr); }

void sleepNoteLine(const char* line, size_t len) {
    long s = parseHibernateSeconds(line, len);
    if (s > 0) plan.noteHibernate(nowS(), s);
    // A session the dongle itself provoked prints the very lines that mean
    // attendance, so it would postpone the sleep by the whole days rule.
    if (!provoked && isBikeAttended(line, len) && clockValid()) { lastAttendedS = nowS(); attendedDirty = true; }
    // Storage mode is whatever the MBB said last; a key-on forgets it until
    // the MBB says it again at its next wake, so the dongle stays reachable
    // for the hour after a ride.
    int m = storageModeFromLine(line, len);
    if (isKeyOn(line, len)) m = 0; else if (m == 0) m = storage;
    if (m != storage) { storage = m; storageNote = true; }
}

// The bike counts as unattended once N days have passed since a top-up, a
// cellular answer or a key-on, or at once when the MBB says it is in storage mode, the owner's own
// statement that it is parked. With no such event ever seen, the count runs
// from the clock's first fix, so a fresh board does not sleep on day one.
static bool unattended() {
    if (afterDays == 0 || storage > 0) return true;
    if (!clockValid() || !lastAttendedS) return false;
    return nowS() - lastAttendedS > (long)afterDays * 86400L;
}

// One light sleep. False when none happened: pin 8 came up between the
// decision and the sleep, or the timer was refused, and a sleep with pin 8
// as the only way back is never taken.
static bool doSleep(long seconds, long untilWakeS) {
    if (mbbLineHigh()) return false;
    Serial.printf("sleep: %ld s of the %ld s until the MBB is due\n", seconds, untilWakeS);
    Serial.flush();
    storeTick(true);   // whatever is pending, while it is quiet
    wifiSuspend();
    sysFeedWatchdog();
    esp_err_t r = esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    uint32_t before = millis();
    if (r == ESP_OK) {
        gpio_wakeup_enable((gpio_num_t)PIN_MBB_RX, GPIO_INTR_HIGH_LEVEL);   // pin 8 rising: the MBB is up
        esp_sleep_enable_gpio_wakeup();
        r = mbbLineHigh() ? ESP_ERR_SLEEP_REJECT : esp_light_sleep_start();   // a high level at entry is a reject either way
        gpio_wakeup_disable((gpio_num_t)PIN_MBB_RX);
    }
    sysFeedWatchdog();
    if (r != ESP_OK) {
        Serial.printf("sleep: not slept, %s\n", esp_err_to_name(r));
        wifiResume();
        return false;
    }
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool onTimer = cause == ESP_SLEEP_WAKEUP_TIMER;
    lastWake = onTimer ? "timer" : cause == ESP_SLEEP_WAKEUP_GPIO ? "pin 8" : "other";
    lastSleptS = (millis() - before) / 1000;   // the planned time as the sleep clock counted it; the real one shows in the clock's next NTP step
    sleeps++;
    plan.slept(onTimer);
    clockSlept();
    Serial.printf("sleep: woke on %s after %lu s\n", lastWake, (unsigned long)lastSleptS);
    wifiResume();
    String note = clockStamp() + " dongle: slept " + String(lastSleptS) + " s, woke on " + lastWake;
    storeAppend(note, false);
    return true;
}

void sleepNoteProvokedWake() { provoked = true; }

void sleepTick(bool mbbAwake, bool busy) {
    if (!mbbAwake) provoked = false;   // the session the wake provoked is over
    uint32_t now = millis();
    if (mbbAwake != wasAwake) {
        wasAwake = mbbAwake;
        if (!mbbAwake) asleepSinceMs = now;
    }
    if (storageNote) {
        storageNote = false;
        storeAppend(clockStamp() + " dongle: storage mode " + (storage > 0 ? "on" : storage < 0 ? "off" : "unknown until the MBB says it again"), false);
    }
    if (!lastAttendedS && clockValid()) { lastAttendedS = nowS(); attendedDirty = true; }
    if (attendedDirty && !mbbAwake) {   // a flash write, so only while the MBB sleeps
        attendedDirty = false;
        Preferences p;
        if (p.begin("dongle", false)) { p.putLong("attended", lastAttendedS); p.end(); }
    }
    if (!enabled || mbbAwake || busy) return;
    if (!unattended()) return;
    if (now - asleepSinceMs < graceMs) return;
    long until = plan.until(nowS(), SLEEP_FALLBACK_S);
    long s = plan.next(nowS(), SLEEP_FALLBACK_S, SLEEP_MARGIN_PCT, SLEEP_MIN_S);
    if (s == 0) return;   // the MBB is due, or this announcement has had its sleep: up for it
    doSleep(s, until);
    asleepSinceMs = millis();   // a fresh grace: the MBB is due and a pull may want the files; after a refusal, pin 8 was up
}

String sleepStatusJson() {
    long due = plan.haveHib ? plan.until(nowS(), 0) : -1;
    long since = lastAttendedS && clockValid() ? nowS() - lastAttendedS : -1;
    return String("{\"enabled\":") + (enabled ? "true" : "false") + ",\"after_days\":" + String(afterDays) +
           ",\"attended_age_s\":" + String(since) + ",\"storage\":\"" + (storage > 0 ? "on" : storage < 0 ? "off" : "unknown") +
           "\",\"armed\":" + (enabled && unattended() ? "true" : "false") +
           ",\"count\":" + String(sleeps) +
           ",\"last_wake\":\"" + lastWake + "\",\"last_slept_s\":" + String(lastSleptS) +
           ",\"mbb_wake_due_s\":" + String(due) + ",\"slept_for_due\":" + (plan.sleptForHib ? "true" : "false") + "}";
}
