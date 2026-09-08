#pragma once
#include <Arduino.h>

void netPrepare();   // before the capture task starts: the raw stream buffer must exist
void netBegin();
void netTick();
void netPushRaw(const uint8_t* data, size_t len);   // MBB bytes for the TCP console clients
int netConsoleClients();
bool netBusy();        // a console client, or a request other than a status check in the last 30 s
void netSuspend();     // services down, WiFi off, for a sleep
void netResume();      // WiFi back on; the join brings the services up
const char* netName();   // zero-dongle-XXXX, from the MAC
String netMac();

// From main.cpp.
const char* sysResetReason();
bool sysWatchdogArmed();
void sysFeedWatchdog();
void sysTickCapture();   // capture housekeeping, safe to call from an HTTP handler
String sysLoopMaxJson();   // the longest single loop pass of each stage since boot, in ms
