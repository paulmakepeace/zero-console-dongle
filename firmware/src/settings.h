#pragma once
#include <Arduino.h>

// What the setup page and /api/settings apply and save: timezone, NTP
// server, sleep on or off, the poll interval and the days before sleeping,
// plus two bench knobs that are applied and never saved. Nothing needs a
// restart. The owners read their own saved values at boot; this owner
// loads the two that are nobody else's.
void settingsBegin();
bool settingsApply(const String& tz, const String& ntp, const String& sleep, const String& poll, const String& days,
                   const String& grace = String(), const String& use = String());   // false: a value too long, nothing applied
bool settingsApplyPushUrl(const String& url);   // empty turns the push off; false: not an http URL, nothing applied
String settingsJson();
const char* settingsTz();
const char* settingsNtp();
const char* settingsPushUrl();
