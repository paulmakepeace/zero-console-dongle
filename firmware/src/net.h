#pragma once
#include <Arduino.h>

void netPrepare();   // before the capture task starts: the raw stream buffer must exist
void netBegin();
void netTick();
void netPushRaw(const uint8_t* data, size_t len);   // MBB bytes for the TCP console clients
const char* netName();   // zero-dongle-XXXX, from the MAC
String netMac();

// From main.cpp.
const char* sysResetReason();
bool sysWatchdogArmed();
void sysFeedWatchdog();
void sysTickCapture();   // capture housekeeping, safe to call from an HTTP handler
