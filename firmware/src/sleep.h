#pragma once
#include <Arduino.h>

// Light sleep between MBB sessions, timed from the MBB's own announcement so
// the dongle is up before the MBB boots; pin 8 rising wakes it regardless.
void sleepBegin(bool enabled, uint32_t afterDays, long lastAttendedS);
void sleepSetEnabled(bool on);
bool sleepEnabled();
void sleepSetAfterDays(uint32_t days);   // 0: whenever the MBB sleeps; N: only once the bike has gone N days without a 12 V top-up or a key-on
// A bench knob, not persisted: the grace before a sleep. Boot restores the default.
void sleepSetGraceMs(uint32_t graceMs);
uint32_t sleepGraceMs();
uint32_t sleepAfterDays();
void sleepNoteLine(const char* line, size_t len);   // watches for the Hibernating line
void sleepTick(bool mbbAwake, bool busy);            // busy: a client, a transfer, or a poll in progress
String sleepStatusJson();
