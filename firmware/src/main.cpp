// Zero console dongle, phase 1: capture the MBB console to flash whenever the
// MBB is awake, serve the files over WiFi, and offer a TCP console.
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "sys.h"
#include "settings.h"
#include "wlan.h"
#include "http.h"
#include "console.h"
#include "poller.h"
#include "sleep.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

static bool wdtArmed = false;
bool sysWatchdogArmed() { return wdtArmed; }

// The longest single pass of each stage of the loop task since boot: a
// stall here is what an HTTP client sees as a timeout.
enum { ST_CAPTURE, ST_NET, ST_POLLER, ST_SLEEP, ST_N };
static const char* const stageName[ST_N] = {"capture", "net", "poller", "sleep"};
static uint32_t stageMaxMs[ST_N];
String sysLoopMaxJson() {
    String s = "{";
    for (int i = 0; i < ST_N; i++) s += String(i ? ",\"" : "\"") + stageName[i] + "\":" + String(stageMaxMs[i]);
    return s + "}";
}
static void timed(int stage, void (*fn)()) {
    uint32_t t = millis();
    fn();
    uint32_t d = millis() - t;
    if (d > stageMaxMs[stage]) stageMaxMs[stage] = d;
}

const char* sysResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

static void onLine(const char* line, size_t len) {
    sleepNoteLine(line, len);   // the attended signals and the hibernate line, whatever else the line is
    if (pollerConsumeLine(line, len)) return;   // a command's output: kept by the poller, not the log
    clockMaybeSetFromMbb(line, len);
    storeAppend(clockStamp() + " " + line, memcmp(line, "dongle:", 7) != 0);
}

static void onRaw(const uint8_t* data, size_t len) {
    consolePushRaw(data, len);   // from the capture task; the stream buffer is lock-free for one writer
}

static void onState(bool awake) {
    Serial.printf("mbb: %s\n", awake ? "awake" : "asleep");
    storeNoteEdge(awake);
    if (!awake) storeSessionClose();   // a session opens on its first line, not on the edge
}

static void onClockNote(const char* note) {
    storeAppend(clockStamp() + " " + note, false);
}

void sysFeedWatchdog() { if (wdtArmed) esp_task_wdt_reset(); }

// Capture housekeeping that a long HTTP transfer must keep running.
void sysTickCapture() {
    sysFeedWatchdog();
    clockTick();   // an NTP fix that has landed is used by the lines in this pass
    mbbTick(onLine, onState);
    storeTick(millis() - mbbLastByteMs() > IDLE_COMMIT_MS);
}

void setup() {
    mbbPinsSafe();   // before anything slow: pin 9 is the MBB's wake pin
    Serial.begin(CONSOLE_BAUD);
    delay(100);
    const char* reason = sysResetReason();
    Serial.printf("zero-dongle fw " FW_VERSION ", reset: %s\n", reason);   // the board's own name follows once the MAC is read

    // A hung loop or capture task reboots with a recorded reason instead of
    // sitting dark. Long legitimate work feeds it through sysFeedWatchdog().
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = LOOP_WDT_S * 1000;
    wdt.idle_core_mask = 1;   // keep the core's idle-task check on CPU0
    wdt.trigger_panic = true;
    esp_err_t e = esp_task_wdt_reconfigure(&wdt);   // the core has already started it
    if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&wdt);
    if (e == ESP_OK) e = esp_task_wdt_add(NULL);
    wdtArmed = (e == ESP_OK);
    if (!wdtArmed) Serial.printf("watchdog: not armed (%s)\n", esp_err_to_name(e));

    storeBegin(reason);
    sysFeedWatchdog();
    Preferences p;
    p.begin("dongle", true);
    bool sleepOn = p.isKey("sleep") ? p.getBool("sleep") : true;
    uint32_t sleepDays = p.isKey("sleep_days") ? p.getUInt("sleep_days") : SLEEP_AFTER_DAYS;
    long attended = p.isKey("attended") ? p.getLong("attended") : 0;
    uint32_t pollS = p.isKey("poll") ? p.getUInt("poll") : POLL_INTERVAL_S;
    p.end();
    settingsBegin();
    sleepBegin(sleepOn, sleepDays, attended);
    pollerBegin(pollS);
    clockBegin(settingsTz(), settingsNtp(), onClockNote);
    consolePrepare();   // the raw stream buffer exists before the capture task can push into it
    if (!mbbBegin(onRaw)) Serial.println("mbb: capture not running");
    httpBegin();   // routes only; the server starts once WiFi is up
    wifiBegin();
    sysFeedWatchdog();
}

void loop() {
    timed(ST_CAPTURE, sysTickCapture);   // lines, markers and edges, in order, on this task
    timed(ST_NET, []() { wifiTick(); httpTick(); consoleTick(); });
    timed(ST_POLLER, []() { pollerTick(mbbAwake(), consoleClients() > 0); });
    timed(ST_SLEEP, []() { sleepTick(mbbAwake(), consoleClients() > 0 || httpBusy() || wifiBusy() || pollerActive() || mbbTxAttached()); });
    delay(2);
}
