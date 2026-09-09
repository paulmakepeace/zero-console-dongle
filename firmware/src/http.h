#pragma once
#include <Arduino.h>

// The web server: status, the log files, settings, the command outputs and
// firmware updates. Routes are set once at boot; the server itself runs
// only on the home network, started and stopped by the WiFi owner.
void httpBegin();
void httpStart();
void httpStop();
void httpTick();
bool httpBusy();   // a page opened or an action taken in the last ten minutes; a page's own refreshes do not count
void httpSetUseMs(uint32_t ms);   // a bench knob, not persisted: the use window. Boot restores the default.
uint32_t httpUseMs();
