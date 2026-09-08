// Zero console dongle, phase 1: capture the MBB console to flash whenever the
// MBB is awake, serve the files over WiFi, and offer a TCP console.
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "net.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

static bool wdtArmed = false;
bool sysWatchdogArmed() { return wdtArmed; }

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
    clockMaybeSetFromMbb(line, len);
    storeAppend(clockStamp() + " " + line, memcmp(line, "dongle:", 7) != 0);
}

static void onRaw(const uint8_t* data, size_t len) {
    netPushRaw(data, len);   // from the capture task; the stream buffer is lock-free for one writer
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
    mbbTick(onLine, onState);
    storeTick(millis() - mbbLastByteMs() > IDLE_COMMIT_MS);
    clockTick();
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
    String tz = p.isKey("tz") ? p.getString("tz") : String(TZ_DEFAULT);
    String ntp = p.isKey("ntp") ? p.getString("ntp") : String(NTP_SERVER);
    p.end();
    clockBegin(tz.c_str(), ntp.c_str(), onClockNote);
    netPrepare();   // the raw stream buffer exists before the capture task can push into it
    if (!mbbBegin(onRaw)) Serial.println("mbb: capture not running");
    netBegin();
    sysFeedWatchdog();
}

void loop() {
    sysTickCapture();   // lines, markers and edges, in order, on this task
    netTick();
    delay(2);
}
