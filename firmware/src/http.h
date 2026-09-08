#pragma once
#include <Arduino.h>

// The web server: status, the log files, settings, the command outputs and
// firmware updates. Routes are set once at boot; the server itself runs
// only on the home network, started and stopped by the WiFi owner.
void httpBegin();
void httpStart();
void httpStop();
void httpTick();
bool httpBusy();   // a request other than a status check in the last ten minutes
