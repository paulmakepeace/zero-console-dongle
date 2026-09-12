// Zero console dongle: capture the MBB console to flash whenever the MBB is
// awake, poll it for its state, serve the files over WiFi, offer a TCP
// console, and sleep between its sessions once the bike is unattended.
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "sys.h"
#include "settings.h"
#include "wlan.h"
#include "http.h"
#include "console.h"
#include "poller.h"
#include "readings.h"
#include "sleep.h"
#include "push.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_mac.h"

// The board's name, derived once from the eFuse MAC before anything else
// needs it: every owner that names the board takes it from here.
static char nodeName[32];
const char* sysNodeName() { return nodeName; }
static void sysIdentity() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // valid before the WiFi driver starts
    snprintf(nodeName, sizeof nodeName, "%s-%02x%02x", DONGLE_NAME, mac[4], mac[5]);
    Serial.printf("zero-dongle: this board is %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", nodeName,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool wdtArmed = false;
bool sysWatchdogArmed() { return wdtArmed; }

// The longest single pass of each stage of the loop task since boot: a
// stall here is what an HTTP client sees as a timeout.
enum { ST_CAPTURE, ST_NET, ST_POLLER, ST_SLEEP, ST_N };
static const char* const stageName[ST_N] = {"capture", "net", "poller", "sleep"};
static uint32_t stageMaxMs[ST_N];
static bool netUntimed = false;   // a long transfer is not a stall: it must not become the net stage's maximum
void sysNetUntimed() { netUntimed = true; }
String sysLoopMaxJson() {
    String s = "{";
    for (int i = 0; i < ST_N; i++) s += String(i ? ",\"" : "\"") + stageName[i] + "\":" + String(stageMaxMs[i]);
    return s + "}";
}
static void timed(int stage, void (*fn)()) {
    uint32_t t = millis();
    fn();
    uint32_t d = millis() - t;
    if (stage == ST_NET && netUntimed) { netUntimed = false; return; }
    if (d > stageMaxMs[stage]) stageMaxMs[stage] = d;
}

const char* sysResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

static void onLine(const char* line, size_t len) {
    clockMaybeSetFromMbb(line, len);   // first: the owners below anchor their state to the wall clock this line may put right
    sleepNoteLine(line, len);   // the attended signals and the hibernate line, whatever else the line is
    pollerConsumeLine(line, len);   // a command's output is kept by the poller for the API, and logged below like any console traffic
    readingsNoteLine(line, len);    // the figures owners asked for, from whatever carries them
    storeAppend(clockStamp() + " " + line, memcmp(line, "dongle:", 7) != 0);
}

static void onRaw(const uint8_t* data, size_t len) {
    consolePushRaw(data, len);   // from the capture task; the stream buffer is lock-free for one writer
}

static void onState(bool awake) {
    // The console print is coalesced to at most one line a second: a wedge floods
    // this edge at ~120 KB/s, and unlimited printing blocks the loop on the 115200
    // console drain, which is a large part of how the board goes dark. Rate-limited
    // it stays reachable, so /api/status uart.awake_edges is readable during the
    // storm; a summary line reports the burst instead of thousands of identical ones.
    static uint32_t lastPrintMs = (uint32_t)0 - 1000, suppressed = 0;   // a second in the past at boot, so the first edge prints whenever it lands
    uint32_t now = millis();
    if (now - lastPrintMs >= 1000) {
        if (suppressed) Serial.printf("mbb: %s (+%lu more edges since the last line)\n", awake ? "awake" : "asleep", (unsigned long)suppressed);
        else Serial.printf("mbb: %s\n", awake ? "awake" : "asleep");
        lastPrintMs = now;
        suppressed = 0;
    } else suppressed++;
    storeNoteEdge(awake);
    if (!awake) { storeSessionClose(); pushRequest(); }   // a session opens on its first line, not on the edge; a closed one is ready to push
}

static void onClockNote(const char* note) {
    storeAppend(clockStamp() + " " + note, false);
}

void sysFeedWatchdog() { if (wdtArmed) esp_task_wdt_reset(); }

// Capture housekeeping that a long HTTP transfer must keep running.
// Called from the loop and re-entrantly from a transfer or an upload, so it
// times itself: the capture done inside the net stage counts here, where a
// stall would be read, rather than disappearing into it.
void sysTickCapture() {
    uint32_t t = millis();
    sysFeedWatchdog();
    clockTick();   // an NTP fix that has landed is used by the lines in this pass
    mbbTick(onLine, onState);
    storeTick(millis() - mbbLastByteMs() > IDLE_COMMIT_MS);
    uint32_t d = millis() - t;
    if (d > stageMaxMs[ST_CAPTURE]) stageMaxMs[ST_CAPTURE] = d;
}

void setup() {
    mbbPinsSafe();   // before anything slow: pin 9 is the MBB's wake pin
    Serial.begin(CONSOLE_BAUD);
    delay(100);
    const char* reason = sysResetReason();
    Serial.printf("zero-dongle fw " FW_VERSION ", reset: %s\n", reason);
    sysIdentity();

    // A hung loop or capture task reboots with a recorded reason instead of
    // sitting dark. Long legitimate work feeds it through sysFeedWatchdog().
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = LOOP_WDT_S * 1000;
    wdt.idle_core_mask = 1;   // keep the core's idle-task check on CPU0
    wdt.trigger_panic = true;
    esp_err_t e = esp_task_wdt_reconfigure(&wdt);   // the core has already started it
    if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&wdt);
    if (e == ESP_OK) e = esp_task_wdt_add(NULL);
    wdtArmed = (e == ESP_OK);
    if (!wdtArmed) Serial.printf("watchdog: not armed (%s)\n", esp_err_to_name(e));

    storeBegin(reason);
    sysFeedWatchdog();
    Preferences p;
    p.begin("dongle", true);
    bool sleepOn = p.isKey("sleep") ? p.getBool("sleep") : true;
    uint32_t sleepDays = p.isKey("sleep_days") ? p.getUInt("sleep_days") : SLEEP_AFTER_DAYS;
    long attended = p.isKey("attended") ? p.getLong("attended") : 0;
    uint32_t pollS = p.isKey("poll") ? p.getUInt("poll") : POLL_INTERVAL_S;
    p.end();
    settingsBegin();
    pushBegin(settingsPushUrl());
    sleepBegin(sleepOn, sleepDays, attended);
    pollerBegin(pollS);
    clockBegin(settingsTz(), settingsNtp(), onClockNote);
    consolePrepare();   // the raw stream buffer exists before the capture task can push into it
    if (!mbbBegin(onRaw)) Serial.println("mbb: capture not running");
    httpBegin();   // routes only; the server starts once WiFi is up
    wifiBegin();
    sysFeedWatchdog();
}

void loop() {
    sysTickCapture();   // lines, markers and edges, in order, on this task; it times itself
    timed(ST_NET, []() { wifiTick(); httpTick(); consoleTick(); pushTick(); });
    timed(ST_POLLER, []() { pollerTick(mbbAwake(), consoleClients() > 0); });
    timed(ST_SLEEP, []() { sleepTick(mbbAwake(), consoleClients() > 0 || httpBusy() || wifiBusy() || pollerActive() || mbbTxAttached() || pushBusy()); });
    delay(2);
}
