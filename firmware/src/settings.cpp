// The settings owner: what is stored in flash and what applying a setting
// means for the owner it belongs to.
#include "settings.h"
#include "config.h"
#include "clock.h"
#include "sleep.h"
#include "poller.h"
#include "http.h"
#include "push.h"
#include "util.h"
#include "sys.h"
#include "pure/push_url.h"
#include <Preferences.h>

static String tzSetting, ntpSetting, pushSetting;

void settingsBegin() {
    Preferences p;
    p.begin("dongle", true);
    tzSetting = p.isKey("tz") ? p.getString("tz") : String(TZ_DEFAULT);
    ntpSetting = p.isKey("ntp") ? p.getString("ntp") : String(NTP_SERVER);
    pushSetting = p.isKey("push") ? p.getString("push") : String();
    p.end();
}

const char* settingsTz() { return tzSetting.c_str(); }
const char* settingsNtp() { return ntpSetting.c_str(); }
const char* settingsPushUrl() { return pushSetting.c_str(); }

String settingsJson() {
    return "{\"tz\":\"" + jsonEscape(tzSetting) + "\",\"ntp\":\"" + jsonEscape(ntpSetting) +
           "\",\"push_url\":\"" + jsonEscape(pushSetting) + "\",\"sleep\":" + (sleepEnabled() ? "1" : "0") + ",\"sleep_days\":" + String(sleepAfterDays()) + ",\"poll\":" + String(pollerInterval()) +
           ",\"sleep_grace\":" + String(sleepGraceMs() / 1000) + ",\"use_s\":" + String(httpUseMs() / 1000) + "}";
}

bool settingsApply(const String& tz, const String& ntp, const String& sleep, const String& poll, const String& days, const String& grace, const String& use, const String& push) {
    if (tz.length() > 63 || ntp.length() > 64) return false;   // what the clock can hold
    String pushUrl = push == "off" ? String() : push;   // empty is not given, like every other field; off clears it
    PushUrl parsed;
    if (pushUrl.length() && (pushUrl.length() > PUSH_URL_MAX || !parsePushUrl(pushUrl.c_str(), pushUrl.length(), parsed))) return false;
    Preferences p;
    p.begin("dongle", false);
    if (tz.length() && tz != tzSetting) { tzSetting = tz; p.putString("tz", tz); }
    if (ntp.length() && ntp != ntpSetting) { ntpSetting = ntp; p.putString("ntp", ntp); }
    if (sleep == "0" || sleep == "1") { sleepSetEnabled(sleep == "1"); p.putBool("sleep", sleep == "1"); }
    if (poll.length() && poll.toInt() >= 0 && poll.toInt() < 100000) { pollerSetInterval(poll.toInt()); p.putUInt("poll", poll.toInt()); }
    if (days.length() && days.toInt() >= 0 && days.toInt() < 1000) { sleepSetAfterDays(days.toInt()); p.putUInt("sleep_days", days.toInt()); }
    // Bench knobs, applied but never saved: the grace before a sleep and the use window, in seconds.
    if (grace.length() && grace.toInt() >= 5 && grace.toInt() <= 3600) sleepSetGraceMs(grace.toInt() * 1000UL);
    if (use.length() && use.toInt() >= 5 && use.toInt() <= 86400) httpSetUseMs(use.toInt() * 1000UL);
    if (push.length() && pushUrl != pushSetting) { pushSetting = pushUrl; p.putString("push", pushUrl); pushSetUrl(pushUrl); }
    p.end();
    clockApplySettings(tzSetting.c_str(), ntpSetting.c_str());   // live; no restart
    Serial.println("settings: applied");
    return true;
}
