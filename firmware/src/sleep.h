#pragma once
#include <Arduino.h>

// Light sleep between MBB sessions, timed from the MBB's own announcement so
// the dongle is up before the MBB boots; pin 8 rising wakes it regardless.
void sleepBegin(bool enabled);
void sleepSetEnabled(bool on);
bool sleepEnabled();
void sleepNoteLine(const char* line, size_t len);   // watches for the Hibernating line
void sleepTick(bool mbbAwake, bool busy);            // busy: a client, a transfer, or a poll in progress
String sleepStatusJson();
