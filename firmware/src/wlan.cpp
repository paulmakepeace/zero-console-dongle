// The WiFi owner: the join to the home network, the setup network and when
// it is worth raising, mDNS, the services that follow the join, and the
// driver's stop and start around a sleep. Nothing here blocks: the join is
// the driver's own, the setup network is a soft AP beside it with our own
// page, and the two are up together until the join lands. Everything runs
// on the loop task except the WiFi event callbacks, which count.
#include "wlan.h"
#include "config.h"
#include "clock.h"
#include "settings.h"
#include "http.h"
#include "console.h"
#include "sys.h"
#include "util.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include <atomic>

static bool servicesUp = false;
static bool mdnsUp = false;
static bool setupUp = false;          // the soft AP with the setup page
static DNSServer setupDns;            // on the setup network every name is the board, so a phone's connectivity probe lands on the setup page
static uint32_t setupRaisedMs = 0;
static uint32_t setupDownAtMs = 0;    // after a join the setup network stays a moment, so its page can say where the board went
static uint32_t lastRetryMs = 0;
static uint32_t lastMdnsMs = 0;
static uint32_t unjoinedSinceMs = 0;  // with credentials and no join: when the wait began
static bool credsSaved = false;       // the driver holds a network; a join proves it too
static bool keyProven = true;         // the saved key has joined before; a key just typed has not, and a refusal erases it as a typo
static bool driverStopped = false;    // esp_wifi_stop for a sleep, not yet restarted
static uint32_t resumeFailures = 0;

// Written by the WiFi event task, read by the loop task.
static volatile uint32_t wifiDisconnects = 0;
static volatile uint8_t lastReason = 0;   // the driver's reason for the last disconnect: 15 and 2 are what a wrong password looks like
static volatile uint8_t refusals = 0;
static volatile bool wrongKey = false;   // set from the event task, read and cleared by the loop task   // the last refusal was the key itself, not a link too weak to finish the handshake     // consecutive disconnects of the password-refused kind
static std::atomic<uint32_t> ipEvents{0};   // joins seen by the event task; the loop counts them down
static uint32_t ipEventsSeen = 0;

static bool driverHasNetwork() {
    wifi_config_t c;
    return esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0] != 0;
}

String wifiMac() { return WiFi.macAddress(); }

// The erase waits for the station to drop; checked, and tried again.
bool wifiResetCredentials() {
    for (int i = 0; i < 3 && driverHasNetwork(); i++) WiFi.disconnect(true, true, 2000);
    credsSaved = driverHasNetwork();
    return !credsSaved;
}

// The setup network counts as use while someone is on it, or for its first
// ten minutes; an unprovisioned board then sleeps like any other, and its
// setup network returns at each wake.
bool wifiBusy() {
    return setupUp && (WiFi.softAPgetStationNum() > 0 || millis() - setupRaisedMs < SETUP_NET_MS);
}

String wifiStatusJson() {
    return "{\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
           ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"disconnects\":" + String(wifiDisconnects) +
           ",\"last_reason\":" + String(lastReason) +
           ",\"mdns\":" + (mdnsUp ? "true" : "false") + ",\"setup_network\":" + (setupUp ? "true" : "false") +
           ",\"resume_failures\":" + String(resumeFailures) + "}";
}

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

// The setup network: our own page on a soft AP beside the station, so a
// board with no network, or one that cannot join its network, can be told
// one from a phone. It comes down at the join.
// While the setup network is up the station keeps still: every attempt it
// makes drags the setup network onto the other channel and a phone mid-join
// gives up. A key that has joined before is tried once every five minutes
// meanwhile, since a marginal link can time out a handshake too; a key
// that never joined is a typo, erased on the first refusal.
static void staQuiet(bool quiet) {
    WiFi.setAutoReconnect(!quiet);
    if (quiet) esp_wifi_disconnect();
}

static void startSetup(const char* why) {
    if (setupUp) return;
    setupUp = true;
    setupDownAtMs = 0;
    staQuiet(true);
    setupRaisedMs = millis() ? millis() : 1;
    WiFi.mode(WIFI_AP_STA);
    IPAddress ap(192, 168, 4, 1);
    WiFi.softAPConfig(ap, ap, IPAddress(255, 255, 255, 0), IPAddress(192, 168, 4, 2), ap);   // the board is the DNS it hands out, so a phone's probe lands here
    WiFi.softAP(sysNodeName());   // open: it is up for minutes, on a bike, and goes down at the join
    setupDns.setTTL(0);
    setupDns.start(53, "*", ap);
    httpStart();   // the setup page; the console waits for the home network
    Serial.printf("wifi: setup network %s up at %s (%s)\n", sysNodeName(), WiFi.softAPIP().toString().c_str(), why);
}

static void stopSetup() {
    if (!setupUp) return;
    setupUp = false;
    setupDownAtMs = 0;
    setupDns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    staQuiet(false);
    Serial.println("wifi: setup network down");
}

// The home-network services: up on every join, down when the network goes.
static void startServices() {
    if (setupUp && !setupDownAtMs) setupDownAtMs = millis() + 20000;   // the page shows the join and the phone's probe gets its Success first
    if (!servicesUp) {
        servicesUp = true;
        httpStart();
        consoleStart();
        Serial.printf("wifi: services up on %s\n", WiFi.localIP().toString().c_str());
    }
    startMdns();
}

static void stopServices() {
    if (mdnsUp) { MDNS.end(); mdnsUp = false; }   // a resume starts it again; left set, it was never re-announced after a sleep
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
    stopSetup();
    WiFi.disconnect(false, false);   // leave the network cleanly; credentials kept
    esp_wifi_stop();
    driverStopped = true;
}

static void driverStart() {
    esp_err_t e = esp_wifi_start();
    if (e == ESP_OK) {
        driverStopped = false;
        unjoinedSinceMs = millis() ? millis() : 1;
        if (credsSaved) WiFi.begin();   // the stored network; the join brings the services up
        else startSetup("no credentials");   // an unprovisioned board's setup network comes back after a sleep
    }
    else { resumeFailures++; Serial.printf("wifi: did not restart (%s); retrying\n", esp_err_to_name(e)); }
    lastRetryMs = millis();
}

void wifiResume() { driverStart(); }

void wifiJoin(const char* ssid, const char* pass) {
    Serial.printf("wifi: joining %s\n", ssid);
    lastReason = 0;
    refusals = 0;
    unjoinedSinceMs = millis() ? millis() : 1;
    lastRetryMs = millis();
    // The driver refuses a new configuration while an attempt is in flight,
    // so the attempt is cancelled first and the set is tried until it takes.
    esp_wifi_disconnect();
    WiFi.setAutoReconnect(true);   // the driver keeps trying this one until it lands or is refused
    for (int i = 0; i < 20 && WiFi.begin(ssid, pass) == WL_CONNECT_FAILED; i++) delay(100);
    credsSaved = true;   // stored by the driver; the join brings the services up and the setup network down
    keyProven = false;
}

void wifiBegin() {
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(sysNodeName());
    // The event task counts; the loop task reads.
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        uint8_t r = info.wifi_sta_disconnected.reason;
        if (r != WIFI_REASON_ASSOC_LEAVE && r != WIFI_REASON_STA_LEAVING) {   // our own leaving, 8 or 36, is not a disconnect
            wifiDisconnects = wifiDisconnects + 1;
            lastReason = r;
            bool refused = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_AUTH_LEAVE ||
                           r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_CONNECTION_FAIL;
            refusals = refused ? (refusals < 255 ? refusals + 1 : 255) : 0;
            // Only the authentication failures are the key itself. A
            // handshake that timed out is what a weak signal looks like, and
            // erasing on one loses a correct password typed at the edge of
            // the range, which is where someone provisioning a bike stands.
            wrongKey = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_AUTH_LEAVE;
        }
        Serial.printf("wifi: disconnected, reason %d\n", r);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { ipEvents.fetch_add(1); }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.mode(WIFI_STA);
    credsSaved = driverHasNetwork();
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    bool bootButtonHeld = digitalRead(PIN_BOOT_BUTTON) == LOW;
    unjoinedSinceMs = millis() ? millis() : 1;
    lastRetryMs = millis();
    if (credsSaved) WiFi.begin();   // the driver's own join, without waiting for it
    if (bootButtonHeld) startSetup("BOOT button held at power-up");
    else if (!credsSaved) startSetup("no credentials");
}

bool wifiOnSetupNetwork(IPAddress local) { return setupUp && (local == WiFi.softAPIP() || WiFi.status() != WL_CONNECTED); }

void wifiTick() {
    uint32_t now = millis();
    if (setupUp) setupDns.processNextRequest();
    if (setupDownAtMs && (int32_t)(now - setupDownAtMs) >= 0) { setupDownAtMs = 0; stopSetup(); }
    uint32_t joins = ipEvents.load();
    if (joins != ipEventsSeen) {
        ipEventsSeen = joins;
        credsSaved = true;   // a join proves there are credentials, however they got there
        keyProven = true;
        clockNetworkUp();    // on every join, so NTP is not left on a backoff
        startServices();
        Serial.printf("wifi: connected to %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        unjoinedSinceMs = now ? now : 1;
        if (!servicesUp) startServices();
        else if (setupUp && !setupDownAtMs) setupDownAtMs = now + 20000;
        if (servicesUp && !mdnsUp && now - lastMdnsMs > 30000) startMdns();
        return;
    }
    if (servicesUp) stopServices();
    // Retry the saved network every 30 s. A network that stays out of reach
    // for ten minutes, or a password it refuses, gets the setup network up
    // beside the retries, so a phone can fix it; the join takes it down.
    bool refused = refusals >= 1;   // a refused key or a link too weak to finish: either way, put the setup network up beside the retries
    if (wrongKey && !keyProven && credsSaved) {   // the key itself was rejected, and it has never joined: a typo, gone, and the board is unprovisioned again
        Serial.println("wifi: the new password was refused; cleared");
        wifiResetCredentials();
        refusals = 0; wrongKey = false;
        if (!setupUp) startSetup("the password was refused");
        staQuiet(true);
        return;
    }
    if (setupUp && refused && WiFi.getAutoReconnect()) { staQuiet(true); Serial.println("wifi: the password was refused; trying again in five minutes"); }
    uint32_t retryMs = setupUp ? 300000 : 30000;
    if (now - lastRetryMs > retryMs) {
        if (driverStopped) driverStart();   // a resume that failed is tried again, credentials or not: the setup network rides on it too
        else if (credsSaved) { lastRetryMs = now; Serial.println("wifi: retrying the saved network"); WiFi.reconnect(); }
    }
    if (credsSaved && !driverStopped && !setupUp) {
        if (refused) startSetup("the password was refused");
        else if (now - unjoinedSinceMs > SETUP_NET_MS) startSetup("not joined for ten minutes");
    }
}
