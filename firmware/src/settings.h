#pragma once
#include <Arduino.h>

// What the setup page and /api/settings apply and save: timezone, NTP
// server, sleep on or off, the poll interval, the days before sleeping and
// the push URL (off to clear it), plus two bench knobs that are applied and never saved. Nothing needs a
// restart. The owners read their own saved values at boot; this owner
// loads the two that are nobody else's.
void settingsBegin();
bool settingsApply(const String& tz, const String& ntp, const String& sleep, const String& poll, const String& days,
                   const String& grace = String(), const String& use = String(), const String& push = String());   // false: a value refused, nothing applied
String settingsJson();
const char* settingsTz();
const char* settingsNtp();
const char* settingsPushUrl();
