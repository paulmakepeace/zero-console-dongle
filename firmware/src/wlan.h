#pragma once
#include <Arduino.h>

// The radio: the join to the home network, the setup network, mDNS, and
// the services' up and down, which follow the join. Suspend and resume
// stop and start the driver around a light sleep.
void wifiBegin();
void wifiTick();
void wifiSuspend();   // services down, driver stopped, for a sleep
void wifiResume();    // driver back on; the join brings the services up
bool wifiBusy();      // the setup network counts as use while someone is on it or for its first ten minutes
const char* wifiName();   // zero-dongle-XXXX, from the MAC
String wifiMac();
String wifiStatusJson();
bool wifiResetCredentials();   // forget the home network; false if the erase did not take; the caller restarts
