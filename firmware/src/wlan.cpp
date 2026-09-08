// The WiFi owner: the join, the setup network and when it is worth raising,
// mDNS, the services that follow the join, and the driver's stop and start
// around a sleep. Everything here runs on the loop task except the WiFi
// event callback, which owns the disconnect accounting.
#include "wlan.h"
#include "config.h"
#include "clock.h"
#include "settings.h"
#include "http.h"
#include "console.h"
#include "sys.h"
#include "util.h"
#include "sleep.h"
#include "poller.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"
#include <atomic>

static WiFiManager wm;
static WiFiManagerParameter tzParam("tz", "Timezone, POSIX form", "", 48);
static WiFiManagerParameter ntpParam("ntp", "NTP server", "", 64);
static WiFiManagerParameter passParam("setup_pass", "Setup network password, 8+ characters", "", 32);
static WiFiManagerParameter sleepParam("sleep", "Sleep between MBB sessions, 1 or 0", "", 2);
static WiFiManagerParameter pollParam("poll", "Poll the MBB every N seconds, 0 for never", "", 6);
static WiFiManagerParameter daysParam("sleep_days", "Sleep only after N days unattended, 0 for always", "", 4);
static bool servicesUp = false;
static bool mdnsUp = false;
static uint32_t lastRetryMs = 0;
static bool credsSaved = false;       // taken once at boot; the driver may be stopped later
static bool driverStopped = false;    // esp_wifi_stop for a sleep, not yet restarted
static uint32_t resumeFailures = 0;
static uint32_t portalRaisedMs = 0;
static bool portalForAuth = false;

// Written by the WiFi event task, read by the loop task.
static volatile uint32_t wifiDisconnects = 0;
static volatile uint8_t authFailures = 0;   // consecutive authentication-class failures
static std::atomic<uint32_t> ipEvents{0};   // joins seen by the event task; the loop counts them down
static uint32_t ipEventsSeen = 0;

String wifiMac() { return WiFi.macAddress(); }
// resetSettings waits 100 ms for the station to drop and gives up silently
// if it has not; the erase is checked and tried again with a longer wait.
bool wifiResetCredentials() {
    for (int i = 0; i < 3; i++) {
        wm.resetSettings();
        if (!wm.getWiFiIsSaved()) return true;
        WiFi.disconnect(true, true, 2000);
    }
    return !wm.getWiFiIsSaved();
}
// The setup network counts as use while someone is on it, or for its first
// ten minutes; an unprovisioned board then sleeps like any other, and its
// setup network returns at each wake.
bool wifiBusy() {
    return wm.getConfigPortalActive() && (WiFi.softAPgetStationNum() > 0 || millis() - portalRaisedMs < AUTH_PORTAL_MS);
}
String wifiStatusJson() {
    return "{\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
           ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"disconnects\":" + String(wifiDisconnects) +
           ",\"mdns\":" + (mdnsUp ? "true" : "false") + ",\"setup_network\":" + (wm.getConfigPortalActive() ? "true" : "false") +
           ",\"resume_failures\":" + String(resumeFailures) + "}";
}

static void startPortal(const char* why, bool forAuth);
static void startMdns();

// The setup page's fields show the live settings, whenever it is raised: a
// save there applies every field, so a stale one would undo a change made
// through the API since boot.
static void fillPortalFields() {
    tzParam.setValue(settingsTz(), 48);
    ntpParam.setValue(settingsNtp(), 64);
    passParam.setValue(settingsSetupPass(), 32);
    sleepParam.setValue(sleepEnabled() ? "1" : "0", 2);
    pollParam.setValue(String(pollerInterval()).c_str(), 6);
    daysParam.setValue(String(sleepAfterDays()).c_str(), 4);
}

// The home-network services: up on every join, down whenever the setup network is raised.
static void startServices() {
    if (wm.getConfigPortalActive()) wm.stopConfigPortal();   // frees port 80 and leaves AP+STA mode
    if (!servicesUp) {
        servicesUp = true;
        httpStart();
        consoleStart();
        Serial.printf("wifi: services up on %s\n", WiFi.localIP().toString().c_str());
    }
    startMdns();
}

static uint32_t lastMdnsMs = 0;
static void startMdns() {
    if (mdnsUp) return;
    lastMdnsMs = millis();
    mdnsUp = MDNS.begin(sysNodeName());
    if (mdnsUp) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("zero-console", "tcp", CONSOLE_PORT);
    } else {
        Serial.println("wifi: mDNS did not start; retrying in 30 s");
    }
}

static void stopServices() {
    if (!servicesUp) return;
    servicesUp = false;
    consoleStop();
    httpStop();
    Serial.println("wifi: services down");
}

// The driver stays initialised across a sleep: stop and start only, so
// its buffers are never freed and re-allocated into a fragmented heap.
void wifiSuspend() {
    stopServices();
    if (wm.getConfigPortalActive()) wm.stopConfigPortal();
    WiFi.disconnect(false, false);   // leave the network cleanly; credentials kept
    esp_wifi_stop();
    driverStopped = true;
}

static void driverStart() {
    esp_err_t e = esp_wifi_start();
    if (e == ESP_OK) {
        driverStopped = false;
        if (credsSaved) WiFi.begin();   // the stored network; the join brings the services up
        else startPortal("no credentials", false);   // an unprovisioned board's setup network comes back after a sleep
    }
    else { resumeFailures++; Serial.printf("wifi: did not restart (%s); retrying\n", esp_err_to_name(e)); }
    lastRetryMs = millis();
}

void wifiResume() { driverStart(); }

static void startPortal(const char* why, bool forAuth) {
    if (wm.getConfigPortalActive()) return;
    stopServices();   // the setup network carries nothing but the setup page
    Serial.printf("wifi: setup network %s up (%s)\n", sysNodeName(), why);
    portalRaisedMs = millis() ? millis() : 1;
    portalForAuth = forAuth;
    fillPortalFields();
    wm.startConfigPortal(sysNodeName(), settingsSetupPass());
}

static void onParamsSaved() {
    settingsApply(tzParam.getValue(), ntpParam.getValue(), passParam.getValue(), sleepParam.getValue(), pollParam.getValue(), daysParam.getValue());
}

void wifiBegin() {
    WiFi.setAutoReconnect(true);
    // The event task owns the failure accounting; the loop task only reads it.
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        uint8_t r = info.wifi_sta_disconnected.reason;
        if (r != WIFI_REASON_ASSOC_LEAVE) wifiDisconnects = wifiDisconnects + 1;   // our own leaving is not a disconnect
        bool auth = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_CONNECTION_FAIL ||
                    r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT;
        if (auth) { if (authFailures < 255) authFailures = authFailures + 1; }
        else if (r != WIFI_REASON_ASSOC_LEAVE) authFailures = 0;   // our own disconnects do not vote
        Serial.printf("wifi: disconnected, reason %d%s\n", r, auth ? " (authentication class)" : "");
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        authFailures = 0;
        ipEvents.fetch_add(1);
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    Serial.printf("wifi: setup network %s, password %s\n", sysNodeName(), settingsSetupPass());
    fillPortalFields();
    wm.addParameter(&tzParam);
    wm.addParameter(&ntpParam);
    wm.addParameter(&passParam);
    wm.addParameter(&sleepParam);
    wm.addParameter(&pollParam);
    wm.addParameter(&daysParam);
    wm.setSaveParamsCallback(onParamsSaved);
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(20);
    wm.setHostname(sysNodeName());
    wm.setEnableConfigPortal(false);   // we decide when the setup network is worth raising
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    bool bootButtonHeld = digitalRead(PIN_BOOT_BUTTON) == LOW;
    sysFeedWatchdog();
    if (bootButtonHeld) {
        startPortal("BOOT button held at power-up", false);
    } else if (wm.autoConnect(sysNodeName(), settingsSetupPass())) {
        ipEvents.fetch_add(1);   // in case the event fired before the handler was in place
        credsSaved = true;
    } else if (!(credsSaved = wm.getWiFiIsSaved())) {
        startPortal("no credentials", false);
    } else {
        Serial.println("wifi: saved network not reachable; retrying without the setup network");
    }
    sysFeedWatchdog();
}

void wifiTick() {
    wm.process();
    uint32_t joins = ipEvents.load();
    if (joins != ipEventsSeen) {
        ipEventsSeen = joins;
        credsSaved = true;   // a join proves there are credentials, however they got there
        clockNetworkUp();   // on every join, so NTP is not left on a backoff
        startServices();
        Serial.printf("wifi: connected to %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !servicesUp) startServices();
    if (!connected) {
        // A wrong password shows as repeated authentication failures; a network
        // that is out of reach does not raise anything, however long it lasts.
        // A marginal link can fail the handshake three times running too, so a
        // setup network raised this way comes down again after a while.
        if (authFailures >= AUTH_FAILS_FOR_PORTAL && !driverStopped) startPortal("authentication failed repeatedly", true);
        if (wm.getConfigPortalActive() && portalForAuth && millis() - portalRaisedMs > AUTH_PORTAL_MS) {
            Serial.println("wifi: setup network down again; back to retrying the saved network");
            wm.stopConfigPortal();
            authFailures = 0;
            lastRetryMs = millis() - 30000;
        }
        // With no setup network up, retry the saved network every 30 s. With one up
        // the user is in charge and WiFiManager connects when they save.
        if (millis() - lastRetryMs > 30000 && !wm.getConfigPortalActive()) {
            if (driverStopped) driverStart();   // a resume that failed is tried again, credentials or not: the setup network rides on it too
            else if (credsSaved) { lastRetryMs = millis(); Serial.println("wifi: retrying the saved network"); WiFi.begin(); }
        }
    }
    if (connected && servicesUp && !mdnsUp && millis() - lastMdnsMs > 30000) startMdns();
}
