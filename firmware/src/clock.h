#pragma once
#include <Arduino.h>

enum TimeSource { TIME_NONE, TIME_MBB, TIME_NTP };

void clockBegin();
bool clockValid();
TimeSource clockSource();
const char* clockSourceName();
String clockStamp();   // "2026-09-06T09:48:02.343", or "u000123.456" seconds since boot when unset
void clockMaybeSetFromMbb(const char* line, size_t len);
