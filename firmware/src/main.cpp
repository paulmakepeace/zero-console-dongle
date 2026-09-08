// Zero console dongle, phase 1: capture the MBB console to flash whenever the
// MBB is awake, serve the files over WiFi, and offer a TCP console.
#include <Arduino.h>
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "net.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

static void onLine(const char* line, size_t len) {
    clockMaybeSetFromMbb(line, len);
    storeAppend(clockStamp() + " " + line);
}

static void onRaw(const uint8_t* data, size_t len) {
    netPushRaw(data, len);
}

static void onState(bool awake) {
    Serial.printf("mbb: %s\n", awake ? "awake" : "asleep");
    storeNoteEdge(awake);
    if (!awake) storeSessionClose();   // a session opens on its first line, not on the edge
}

static const char* resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        default: return "unknown";
    }
}
const char* lastResetReason = "";

void setup() {
    mbbPinsSafe();   // before anything slow: pin 9 is the MBB's wake pin
    Serial.begin(115200);
    delay(100);
    lastResetReason = resetReason();
    Serial.printf("zero-dongle fw " FW_VERSION ", reset: %s\n", lastResetReason);   // the board's own name follows once the MAC is read
    // A hung loop or capture task reboots with a recorded reason instead of sitting dark.
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = LOOP_WDT_S * 1000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt);
    esp_task_wdt_add(NULL);
    storeBegin();
    clockBegin();
    mbbBegin(onLine, onRaw, onState);
    netBegin();
}

void loop() {
    esp_task_wdt_reset();
    netTick();
    storeTick();
    delay(2);
}
