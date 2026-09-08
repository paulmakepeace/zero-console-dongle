// Zero console dongle, phase 1: capture the MBB console to flash whenever the
// MBB is awake, serve the files over WiFi, and offer a TCP console.
#include <Arduino.h>
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "net.h"

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

void setup() {
    mbbPinsSafe();   // before anything slow: pin 9 is the MBB's wake pin
    Serial.begin(115200);
    delay(100);
    Serial.println("zero-dongle fw " FW_VERSION);   // the board's own name follows once the MAC is read
    storeBegin();
    clockBegin();
    mbbBegin(onLine, onRaw, onState);
    netBegin();
}

void loop() {
    netTick();
    storeTick();
    delay(2);
}
