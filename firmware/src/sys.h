#pragma once
#include <Arduino.h>

// From main.cpp: the board's own housekeeping, for any owner that needs it.
const char* sysNodeName();          // zero-dongle-XXXX, from the MAC: the hostname, the setup network's name, the session header's board
const char* sysResetReason();
bool sysWatchdogArmed();
void sysFeedWatchdog();
void sysTickCapture();     // capture housekeeping, safe to call from an HTTP handler
String sysLoopMaxJson();   // the longest single loop pass of each stage since boot, in ms
