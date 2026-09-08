// The settings owner: what is stored in flash and what applying a setting
// means for the owner it belongs to.
#include "settings.h"
#include "config.h"
#include "clock.h"
#include "sleep.h"
#include "poller.h"
#include "util.h"
#include <Preferences.h>
#include "esp_mac.h"

static String tzSetting, ntpSetting;
static char setupPass[33];

void settingsBegin() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // from the eFuse; valid before the WiFi driver starts
    char defaultPass[16];
    snprintf(defaultPass, sizeof defaultPass, "zero-%02x%02x%02x", mac[3], mac[4], mac[5]);
    Preferences p;
    p.begin("dongle", true);
    tzSetting = p.isKey("tz") ? p.getString("tz") : String(TZ_DEFAULT);
    ntpSetting = p.isKey("ntp") ? p.getString("ntp") : String(NTP_SERVER);
    String pass = p.isKey("setup_pass") ? p.getString("setup_pass") : String(defaultPass);
    p.end();
    strlcpy(setupPass, pass.c_str(), sizeof setupPass);
}

const char* settingsTz() { return tzSetting.c_str(); }
const char* settingsNtp() { return ntpSetting.c_str(); }
const char* settingsSetupPass() { return setupPass; }

String settingsJson() {
    return "{\"tz\":\"" + jsonEscape(tzSetting) + "\",\"ntp\":\"" + jsonEscape(ntpSetting) +
           "\",\"sleep\":" + (sleepEnabled() ? "1" : "0") + ",\"sleep_days\":" + String(sleepAfterDays()) + ",\"poll\":" + String(pollerInterval()) +
           ",\"sleep_grace\":" + String(sleepGraceMs() / 1000) + "}";
}

bool settingsApply(const String& tz, const String& ntp, const String& pass, const String& sleep, const String& poll, const String& days, const String& grace) {
    if (tz.length() > 63 || ntp.length() > 64 || pass.length() > 32) return false;   // what the clock and the setup network can hold
    Preferences p;
    p.begin("dongle", false);
    if (tz.length() && tz != tzSetting) { tzSetting = tz; p.putString("tz", tz); }
    if (ntp.length() && ntp != ntpSetting) { ntpSetting = ntp; p.putString("ntp", ntp); }
    if (pass.length() >= 8 && pass != setupPass) { strlcpy(setupPass, pass.c_str(), sizeof setupPass); p.putString("setup_pass", pass); }
    if (sleep == "0" || sleep == "1") { sleepSetEnabled(sleep == "1"); p.putBool("sleep", sleep == "1"); }
    if (poll.length() && poll.toInt() >= 0 && poll.toInt() < 100000) { pollerSetInterval(poll.toInt()); p.putUInt("poll", poll.toInt()); }
    if (days.length() && days.toInt() >= 0 && days.toInt() < 1000) { sleepSetAfterDays(days.toInt()); p.putUInt("sleep_days", days.toInt()); }
    // A bench knob, applied but never saved: the grace before a sleep, in seconds.
    if (grace.length() && grace.toInt() >= 5) sleepSetGraceMs(grace.toInt() * 1000UL);
    p.end();
    clockApplySettings(tzSetting.c_str(), ntpSetting.c_str());   // live; no restart
    Serial.println("settings: applied");
    return true;
}
