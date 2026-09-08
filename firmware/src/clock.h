#pragma once
#include <Arduino.h>

enum TimeSource { TIME_NONE, TIME_MBB, TIME_NTP };

typedef void (*ClockNoteHandler)(const char* note);   // a line for the log, e.g. a clock step

void clockBegin(const char* tz, const char* ntpServer, ClockNoteHandler onNote);
bool clockValid();
TimeSource clockSource();
const char* clockSourceName();
uint32_t clockNtpAgeS();    // seconds since the last NTP sync, or UINT32_MAX
String clockStamp();        // "2026-09-06T09:48:02.343", or "u000123.456" seconds since boot when unset
void clockMaybeSetFromMbb(const char* line, size_t len);
void clockTick();           // from loop(): reports an NTP sync into the log
void clockNetworkUp();      // WiFi just connected: restart NTP so the first sync is not on a backoff
