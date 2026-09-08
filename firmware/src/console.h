#pragma once
#include <Arduino.h>

// The raw TCP console onto the MBB: the capture task's bytes fanned out to
// the clients, their keystrokes gated onto the transmit pin. It runs only
// on the home network, started and stopped by the WiFi owner.
void consolePrepare();   // before the capture task starts: the raw stream buffer must exist
void consoleStart();
void consoleStop();
void consoleTick();
void consolePushRaw(const uint8_t* data, size_t len);   // from the capture task; the stream buffer is lock-free for one writer
int consoleClients();
String consoleStatusJson();
