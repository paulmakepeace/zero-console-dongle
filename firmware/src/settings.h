#pragma once
#include <Arduino.h>

// The settings kept in flash and applied live: timezone, NTP server, the
// setup network's password, sleep on or off, the poll interval and the
// days before sleeping. The setup page and /api/settings both apply
// through settingsApply; nothing needs a restart.
void settingsBegin();
void settingsApply(const String& tz, const String& ntp, const String& pass, const String& sleep, const String& poll, const String& days, const String& grace = String());
String settingsJson();
const char* settingsTz();
const char* settingsNtp();
const char* settingsSetupPass();
