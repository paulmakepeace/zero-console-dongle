// WiFi with a setup portal, mDNS, the HTTP server for the log files, settings
// and firmware updates, and a raw TCP console onto the MBB. The services
// exist only on the home network: they come up on every join and go down
// whenever the setup network is raised. Everything here runs on the loop
// task except the WiFi event callback, which owns the disconnect accounting.
#include "net.h"
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "util.h"
#include "poller.h"
#include "sleep.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "esp_mac.h"
#include <atomic>

static WiFiManager wm;
static WiFiManagerParameter tzParam("tz", "Timezone, POSIX form", "", 48);
static WiFiManagerParameter ntpParam("ntp", "NTP server", "", 64);
static WiFiManagerParameter passParam("setup_pass", "Setup network password, 8+ characters", "", 32);
static WiFiManagerParameter sleepParam("sleep", "Sleep between MBB sessions, 1 or 0", "", 2);
static WiFiManagerParameter pollParam("poll", "Poll the MBB every N seconds, 0 for never", "", 6);
static WiFiManagerParameter daysParam("sleep_days", "Sleep only after N days unattended, 0 for always", "", 4);
static char setupPass[33];
static WebServer http(HTTP_PORT);
static WiFiServer console(CONSOLE_PORT);
static WiFiClient clients[CONSOLE_CLIENTS];
static StreamBufferHandle_t rawBuf;
static std::atomic<uint32_t> rawDropped{0};   // added on the capture task, taken here
static bool servicesUp = false;
static bool mdnsUp = false;
static uint32_t lastRetryMs = 0;
static char nodeName[32];
static String tzSetting, ntpSetting;
static uint32_t lastHttpMs = 0;
static void touch() { lastHttpMs = millis(); }

// Written by the WiFi event task, read by the loop task.
static volatile uint32_t wifiDisconnects = 0;
static volatile uint8_t authFailures = 0;   // consecutive authentication-class failures
static std::atomic<uint32_t> ipEvents{0};   // joins seen by the event task; the loop counts them down
static uint32_t ipEventsSeen = 0;

// Per console client: CR-LF state, output that did not fit its socket yet,
// and the last time it was told the MBB is asleep.
struct ConsoleState {
    uint8_t prev = 0;
    uint8_t pend[512];
    size_t pendLen = 0;
    uint32_t lost = 0;
    uint32_t asleepNoteMs = 0;
};
static ConsoleState cstate[CONSOLE_CLIENTS];
static size_t inputCursor = 0;

const char* netName() { return nodeName; }
int netConsoleClients() { int n = 0; for (auto& c : clients) if (c && c.connected()) n++; return n; }
bool netBusy() { return netConsoleClients() > 0 || millis() - lastHttpMs < 30000; }
String netMac() { return WiFi.macAddress(); }

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>zero-dongle</title>
<style>body{font:14px system-ui,sans-serif;margin:1em;max-width:60em}pre{background:#f4f4f4;padding:.5em;overflow-x:auto;font-size:12px}table{border-collapse:collapse}td{padding:.1em .8em .1em 0}a{margin-right:1em}</style>
<h2 id=t>zero-dongle</h2><p><a href=/cmd>Command outputs</a></p><table id=s></table>
<h3>Files</h3><div id=fs></div><div id=f></div>
<h3>Last lines</h3><pre id=l></pre>
<h3>Firmware update</h3>
<input type=file id=fw accept=.bin> <button id=go>Flash</button> <span id=fwmsg></span>
<script>
async function j(u){return (await fetch(u)).json()}
function row(t,k,v){const tr=t.insertRow();tr.insertCell().textContent=k;tr.insertCell().textContent=typeof v=='object'?JSON.stringify(v):v}
async function refresh(){
 const s=await j('/api/status'); document.getElementById('t').textContent=s.name;
 const t=document.getElementById('s'); t.textContent=''; for(const [k,v] of Object.entries(s)) row(t,k,v);
 const st=s.store; document.getElementById('fs').textContent=st.files+' file(s), '+(st.bytes/1024).toFixed(0)+' KB on flash of '+(st.fs_total/1024).toFixed(0)+' KB, compressing '+st.ratio+'x since boot'+(st.days_left>=0?', about '+st.days_left+' day(s) of space left at this rate':'');
 const f=await j('/logs'); const d=document.getElementById('f'); d.textContent='';
 for(const x of f){const div=document.createElement('div'); if(x.active){div.textContent=x.name+' '+x.size+' bytes (active, see last lines)'}else{const a=document.createElement('a');a.href='/logs/'+encodeURIComponent(x.name);a.textContent=x.name;div.appendChild(a);div.appendChild(document.createTextNode(' '+x.size+' bytes'))} d.appendChild(div)}
 if(!f.length) d.textContent='none';
 document.getElementById('l').textContent=await (await fetch('/live')).text();
}
document.getElementById('go').onclick=async()=>{
 const f=document.getElementById('fw').files[0]; if(!f) return;
 const fd=new FormData(); fd.append('firmware',f);
 document.getElementById('fwmsg').textContent='uploading '+f.size+' bytes';
 const r=await fetch('/update',{method:'POST',body:fd,headers:{'X-Dongle':'1'}});
 document.getElementById('fwmsg').textContent=await r.text();
};
refresh();setInterval(refresh,5000);
</script>)HTML";

static const char CMD_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>zero-dongle commands</title>
<style>body{font:14px system-ui,sans-serif;margin:1em;max-width:70em}pre{background:#f4f4f4;padding:.5em;overflow-x:auto;font-size:12px;min-height:10em}button{margin:0 .3em .5em 0;padding:.3em .8em}button.on{font-weight:bold;background:#ddd}#age{color:#666}</style>
<p><a href=/>Status</a></p>
<div id=tabs></div>
<div id=age></div>
<pre id=out>loading</pre>
<button id=poll>Poll now</button> <span id=msg></span>
<script>
let cur=null;
async function tabs(){
 const list=await (await fetch('/api/cmd')).json();
 const t=document.getElementById('tabs'); t.textContent='';
 for(const c of list){const b=document.createElement('button'); b.textContent=c.name+(c.age_s<0?' (none)':''); b.className=c.name==cur?'on':''; b.onclick=()=>{cur=c.name;show()}; t.appendChild(b)}
 if(!cur&&list.length){cur=list[0].name}
}
async function show(){
 if(!cur) return;
 const r=await fetch('/api/cmd/'+encodeURIComponent(cur));
 document.getElementById('out').textContent=r.ok?await r.text():('('+r.status+' '+await r.text()+')');
 const a=r.headers.get('X-Age-Seconds'); document.getElementById('age').textContent=a?cur+', '+a+' s ago':'';
 for(const b of document.querySelectorAll('#tabs button')) b.className=b.textContent.startsWith(cur)?'on':'';
}
document.getElementById('poll').onclick=async()=>{const r=await fetch('/api/cmd/poll',{method:'POST',headers:{'X-Dongle':'1'}}); document.getElementById('msg').textContent=await r.text(); setTimeout(()=>{tabs();show()},20000)};
tabs().then(show); setInterval(()=>{tabs();show()},15000);
</script>)HTML";

static int consoleClientCount() {
    int n = 0;
    for (auto& c : clients) if (c && c.connected()) n++;
    return n;
}

static String statusJson() {   // a health check is not use: a watcher must not keep the dongle awake
    size_t total, used;
    storeStats(total, used);
    String s;
    s.reserve(900);
    s += "{\"name\":\"" + String(nodeName) + "\",\"mac\":\"" + netMac() + "\",\"fw\":\"" FW_VERSION "\"";
    s += ",\"uptime_s\":" + String(millis() / 1000);
    s += ",\"boot\":" + String(storeBootCount());
    s += ",\"reset_reason\":\"" + String(sysResetReason()) + "\"";
    s += ",\"watchdog\":" + String(sysWatchdogArmed() ? "true" : "false");
    s += ",\"mbb_awake\":" + String(mbbAwake() ? "true" : "false");
    s += ",\"line_high\":" + String(mbbLineHigh() ? "true" : "false");
    s += ",\"tx_attached\":" + String(mbbTxAttached() ? "true" : "false");
    s += ",\"time\":\"" + jsonEscape(clockStamp()) + "\",\"time_source\":\"" + clockSourceName() + "\"";
    s += ",\"ntp_age_s\":" + String(clockNtpAgeS() == UINT32_MAX ? -1 : (long)clockNtpAgeS());
    s += ",\"wifi\":{\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
         ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"disconnects\":" + String(wifiDisconnects) +
         ",\"mdns\":" + (mdnsUp ? "true" : "false") + ",\"setup_network\":" + (wm.getConfigPortalActive() ? "true" : "false") + "}";
    s += ",\"fs\":{\"total\":" + String(total) + ",\"used\":" + String(used) + ",\"ok\":" +
         (storeOk() ? "true" : "false") + ",\"formats\":" + String(storeFormats()) + "}";
    s += ",\"active\":\"" + jsonEscape(storeActiveName()) + "\"";
    s += "," + storeEdges();
    s += ",\"dropped_lines\":" + String(storeDroppedLines());
    s += ",\"uart\":{\"ok\":" + String(mbbOk() ? "true" : "false") + ",\"overflows\":" + String(mbbOverflows()) +
         ",\"backpressure\":" + String(mbbBackpressure()) + ",\"frame_errors\":" + String(mbbFrameErrors()) +
         ",\"queue_drops\":" + String(mbbQueueDrops()) + "}";
    s += ",\"console\":{\"clients\":" + String(consoleClientCount()) + ",\"dropped_bytes\":" + String(rawDropped.load()) + "}";
    {
        long soc, mv, ma, ah, hi, lo;
        bool have = pollerPack(soc, mv, ma, ah, hi, lo);
        s += ",\"pack\":{\"soc\":" + String(pollerSoc()) + ",\"bike_state\":\"" + jsonEscape(pollerBikeState()) + "\"" +
             ",\"age_s\":" + String(pollerOutputAgeS("status") == UINT32_MAX ? -1 : (long)pollerOutputAgeS("status"));
        if (have) s += ",\"mv\":" + String(mv) + ",\"ma\":" + String(ma) + ",\"ah\":" + String(ah) +
                       ",\"temp_hi_c\":" + String(hi) + ",\"temp_lo_c\":" + String(lo);
        s += "}";
    }
    s += ",\"store\":" + storeMetricsJson();
    s += ",\"esp_temp_c\":" + String(temperatureRead(), 1);   // the die, not the air: it runs some 15 to 20 C above ambient
    s += ",\"poll\":{\"interval_s\":" + String(pollerInterval()) + ",\"active\":" + (pollerActive() ? "true" : "false") + "}";
    s += ",\"sleep\":" + sleepStatusJson();
    s += ",\"heap_free\":" + String(ESP.getFreeHeap()) + ",\"heap_min_free\":" + String(ESP.getMinFreeHeap()) +
         ",\"heap_max_alloc\":" + String(ESP.getMaxAllocHeap());
    s += ",\"stack_free\":{\"loop\":" + String(uxTaskGetStackHighWaterMark(nullptr)) +
         ",\"capture\":" + String(mbbCaptureStackFree()) + "}";
    s += "}";
    return s;
}

// State-changing requests need a header a cross-site form cannot set.
static bool tokenOk() {
    if (http.header("X-Dongle") == "1") return true;
    http.send(403, "text/plain", "missing X-Dongle: 1 header");
    return false;
}

static void pumpConsole();

static void handleCmd(const String& name) {
    touch();
    const String* out = pollerOutput(name.c_str());
    if (!out) {
        bool known = pollerListJson().indexOf("\"name\":\"" + name + "\"") >= 0;
        http.send(known ? 503 : 404, "text/plain", known ? "not polled yet" : "no such command");
        return;
    }
    http.sendHeader("X-Age-Seconds", String(pollerOutputAgeS(name.c_str())));
    http.send(200, "text/plain", *out);
}

static void handleFile() {
    String uri = http.uri();
    if (uri.startsWith("/api/cmd/")) { handleCmd(uri.substring(9)); return; }
    touch();
    if (!uri.startsWith("/logs/")) {
        http.send(404, "text/plain", "not found");
        return;
    }
    String name = uri.substring(6);
    if (name.length() == 0) {
        http.send(404, "text/plain", "not found");
        return;
    }
    if (http.method() == HTTP_GET) {
        if (name == storeActiveName()) {
            http.send(409, "text/plain", "file is active; see /live");
            return;
        }
        bool busy = false;
        File f = storeOpenRead(name, &busy);
        if (!f) {
            if (busy) http.send(503, "text/plain", "no free reader; try again");
            else http.send(404, "text/plain", "no such file");
            return;
        }
        // Chunked by hand so the capture and the console keep running on a slow client.
        http.setContentLength(f.size());
        http.send(200, name.endsWith(".gz") ? "application/gzip" : "text/plain", "");
        WiFiClient c = http.client();
        uint8_t buf[1024];
        bool whole = true;
        while (f.available() && c.connected()) {
            size_t n = f.read(buf, sizeof buf);
            if (n == 0) { whole = false; break; }   // a bad block: do not spin on it
            if (c.write(buf, n) != n) { whole = false; break; }
            sysTickCapture();
            pumpConsole();
        }
        if (!whole) c.stop();   // the promised length will not arrive; say so by closing
        f.close();
        storeReadDone(name);
        return;
    }
    if (http.method() == HTTP_DELETE) {
        if (!tokenOk()) return;
        switch (storeDelete(name)) {
            case STORE_DELETED: http.send(200, "text/plain", "deleted"); break;
            case STORE_NOT_FOUND: http.send(404, "text/plain", "no such file"); break;
            default: http.send(409, "text/plain", "refused: active, being read, or a bad name"); break;
        }
        return;
    }
    http.send(405, "text/plain", "method");
}

static void applySettings(const String& tz, const String& ntp, const String& pass, const String& sleep, const String& poll, const String& days) {
    Preferences p;
    p.begin("dongle", false);
    if (tz.length() && tz != tzSetting) { tzSetting = tz; p.putString("tz", tz); }
    if (ntp.length() && ntp != ntpSetting) { ntpSetting = ntp; p.putString("ntp", ntp); }
    if (pass.length() >= 8 && pass != setupPass) { strlcpy(setupPass, pass.c_str(), sizeof setupPass); p.putString("setup_pass", pass); }
    if (sleep == "0" || sleep == "1") { sleepSetEnabled(sleep == "1"); p.putBool("sleep", sleep == "1"); }
    if (poll.length() && poll.toInt() >= 0 && poll.toInt() < 100000) { pollerSetInterval(poll.toInt()); p.putUInt("poll", poll.toInt()); }
    if (days.length() && days.toInt() >= 0 && days.toInt() < 1000) { sleepSetAfterDays(days.toInt()); p.putUInt("sleep_days", days.toInt()); }
    p.end();
    clockApplySettings(tzSetting.c_str(), ntpSetting.c_str());   // live; no restart
    Serial.println("net: settings applied");
}

static void setupHttp() {
    http.on("/", HTTP_GET, []() { http.send_P(200, "text/html", PAGE); });
    http.on("/api/status", HTTP_GET, []() { http.send(200, "application/json", statusJson()); });
    http.on("/logs", HTTP_GET, []() { touch(); http.send(200, "application/json", storeListJson()); });
    http.on("/live", HTTP_GET, []() { touch(); http.send(200, "text/plain", storeLastLines()); });
    http.on("/api/settings", HTTP_GET, []() {
        http.send(200, "application/json", "{\"tz\":\"" + jsonEscape(tzSetting) + "\",\"ntp\":\"" + jsonEscape(ntpSetting) +
                  "\",\"sleep\":" + (sleepEnabled() ? "1" : "0") + ",\"sleep_days\":" + String(sleepAfterDays()) + ",\"poll\":" + String(pollerInterval()) + "}");
    });
    http.on("/api/settings", HTTP_POST, []() {   // form fields tz, ntp, setup_pass, sleep, poll; any subset
        if (!tokenOk()) return;
        applySettings(http.arg("tz"), http.arg("ntp"), http.arg("setup_pass"), http.arg("sleep"), http.arg("poll"), http.arg("sleep_days"));
        http.send(200, "text/plain", "applied");
    });
    http.on("/cmd", HTTP_GET, []() { touch(); http.send_P(200, "text/html", CMD_PAGE); });
    http.on("/api/cmd", HTTP_GET, []() { touch(); http.send(200, "application/json", pollerListJson()); });
    http.on("/api/cmd/poll", HTTP_POST, []() {
        if (!tokenOk()) return;
        touch();
        pollerRequest();
        http.send(200, "text/plain", "poll requested");
    });
    http.on("/api/wifi/reset", HTTP_POST, []() {
        if (!tokenOk()) return;
        http.send(200, "text/plain", "credentials cleared, rebooting into setup");
        sysTickCapture();   // lines framed but not yet delivered
        storeShutdown();
        wm.resetSettings();
        delay(500);   // let the WiFi task commit the erase before the reset
        ESP.restart();
    });
    http.on("/update", HTTP_POST,
        []() {
            http.sendHeader("Connection", "close");
            if (http.header("X-Dongle") != "1") { http.send(403, "text/plain", "missing X-Dongle: 1 header"); return; }
            http.send(200, "text/plain", Update.hasError() ? "update failed" : "ok, rebooting");
            delay(300);
            if (!Update.hasError()) {
                sysTickCapture();   // lines framed but not yet delivered
                storeShutdown();    // end the file cleanly rather than mid-line
                ESP.restart();
            }
        },
        []() {
            sysTickCapture();   // a slow link can take minutes; feed even while refusing
            HTTPUpload& up = http.upload();
            if (http.header("X-Dongle") != "1") return;
            if (up.status == UPLOAD_FILE_START) {
                Serial.printf("ota: %s\n", up.filename.c_str());
                if (Update.isRunning()) Update.abort();
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Serial.printf("ota: %s\n", Update.errorString());
            } else if (up.status == UPLOAD_FILE_WRITE) {
                if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize)
                    Serial.printf("ota: %s\n", Update.errorString());
            } else if (up.status == UPLOAD_FILE_END) {
                if (!Update.end(true)) Serial.printf("ota: %s\n", Update.errorString());
            } else if (up.status == UPLOAD_FILE_ABORTED) {
                Update.abort();
                Serial.println("ota: upload aborted");
            }
        });
    const char* headers[] = {"X-Dongle"};
    http.collectHeaders(headers, 1);
    http.onNotFound(handleFile);
}

// The home-network services: up on every join, down whenever the setup network is raised.
static void startServices() {
    if (wm.getConfigPortalActive()) wm.stopConfigPortal();   // frees port 80 and leaves AP+STA mode
    if (!servicesUp) {
        servicesUp = true;
        http.begin();
        console.begin();
        console.setNoDelay(true);
        Serial.printf("net: services up on %s\n", WiFi.localIP().toString().c_str());
    }
    if (!mdnsUp) {
        mdnsUp = MDNS.begin(nodeName);
        if (mdnsUp) {
            MDNS.addService("http", "tcp", HTTP_PORT);
            MDNS.addService("zero-console", "tcp", CONSOLE_PORT);
        } else {
            Serial.println("net: mDNS did not start; will retry");
        }
    }
}

static void stopServices() {
    if (!servicesUp) return;
    servicesUp = false;
    for (auto& c : clients) if (c) c.stop();
    console.end();
    http.stop();
    Serial.println("net: services down");
}

void netSuspend() {
    stopServices();
    WiFi.disconnect(true, false);   // radio off, credentials kept
    WiFi.mode(WIFI_OFF);
}

void netResume() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();   // the stored network; the join brings the services up
    lastRetryMs = millis();
}

static void startPortal(const char* why) {
    if (wm.getConfigPortalActive()) return;
    stopServices();   // the setup network carries nothing but the setup page
    Serial.printf("net: setup network %s up (%s)\n", nodeName, why);
    wm.startConfigPortal(nodeName, setupPass);
}

static void onParamsSaved() {
    applySettings(tzParam.getValue(), ntpParam.getValue(), passParam.getValue(), sleepParam.getValue(), pollParam.getValue(), daysParam.getValue());
}

void netPrepare() {
    rawBuf = xStreamBufferCreate(8192, 1);
    if (!rawBuf) Serial.println("net: no memory for the console buffer; console output off");
}

void netBegin() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // from the eFuse; valid before the WiFi driver starts
    snprintf(nodeName, sizeof nodeName, "%s-%02x%02x", DONGLE_NAME, mac[4], mac[5]);
    Serial.printf("net: this board is %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", nodeName,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    WiFi.setAutoReconnect(true);
    // The event task owns the failure accounting; the loop task only reads it.
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        wifiDisconnects = wifiDisconnects + 1;
        uint8_t r = info.wifi_sta_disconnected.reason;
        bool auth = r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_CONNECTION_FAIL ||
                    r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT;
        if (auth) { if (authFailures < 255) authFailures = authFailures + 1; }
        else if (r != WIFI_REASON_ASSOC_LEAVE) authFailures = 0;   // our own disconnects do not vote
        Serial.printf("net: WiFi disconnected, reason %d%s\n", r, auth ? " (authentication class)" : "");
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
        authFailures = 0;
        ipEvents.fetch_add(1);
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    char defaultPass[16];
    snprintf(defaultPass, sizeof defaultPass, "zero-%02x%02x%02x", mac[3], mac[4], mac[5]);
    Preferences p;
    p.begin("dongle", true);
    tzSetting = p.isKey("tz") ? p.getString("tz") : String(TZ_DEFAULT);
    ntpSetting = p.isKey("ntp") ? p.getString("ntp") : String(NTP_SERVER);
    String pass = p.isKey("setup_pass") ? p.getString("setup_pass") : String(defaultPass);
    p.end();
    strlcpy(setupPass, pass.c_str(), sizeof setupPass);
    Serial.printf("net: setup network %s, password %s\n", nodeName, setupPass);
    tzParam.setValue(tzSetting.c_str(), 48);
    ntpParam.setValue(ntpSetting.c_str(), 64);
    passParam.setValue(setupPass, 32);
    sleepParam.setValue(sleepEnabled() ? "1" : "0", 2);
    pollParam.setValue(String(pollerInterval()).c_str(), 6);
    daysParam.setValue(String(sleepAfterDays()).c_str(), 4);
    wm.addParameter(&tzParam);
    wm.addParameter(&ntpParam);
    wm.addParameter(&passParam);
    wm.addParameter(&sleepParam);
    wm.addParameter(&pollParam);
    wm.addParameter(&daysParam);
    wm.setSaveParamsCallback(onParamsSaved);
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(20);
    wm.setHostname(nodeName);
    wm.setEnableConfigPortal(false);   // we decide when the setup network is worth raising
    setupHttp();   // routes only; the server starts once WiFi is up
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    bool bootButtonHeld = digitalRead(PIN_BOOT_BUTTON) == LOW;
    sysFeedWatchdog();
    if (bootButtonHeld) {
        startPortal("BOOT button held at power-up");
    } else if (wm.autoConnect(nodeName, setupPass)) {
        ipEvents.fetch_add(1);   // in case the event fired before the handler was in place
    } else if (!wm.getWiFiIsSaved()) {
        startPortal("no credentials");
    } else {
        Serial.println("net: saved network not reachable; retrying without the setup network");
    }
    sysFeedWatchdog();
}

void netPushRaw(const uint8_t* data, size_t len) {
    if (!rawBuf) return;
    uint8_t tmp[256];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0) continue;
        tmp[k++] = data[i];
        if (k == sizeof tmp) { rawDropped.fetch_add(k - xStreamBufferSend(rawBuf, tmp, k, 0)); k = 0; }
    }
    if (k) rawDropped.fetch_add(k - xStreamBufferSend(rawBuf, tmp, k, 0));
}

static void consoleSend(size_t ci, const uint8_t* data, size_t len) {
    WiFiClient& slot = clients[ci];
    ConsoleState& st = cstate[ci];
    // First whatever is still pending from last time.
    if (st.pendLen) {
        int sent = ::send(slot.fd(), st.pend, st.pendLen, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) slot.stop();
            st.lost += len;
            return;
        }
        if ((size_t)sent < st.pendLen) {
            memmove(st.pend, st.pend + sent, st.pendLen - sent);
            st.pendLen -= sent;
            st.lost += len;
            return;
        }
        st.pendLen = 0;
        if (st.lost) {   // the client is taking data again: say what it missed
            st.pendLen = snprintf((char*)st.pend, sizeof st.pend,
                                  "\n[dongle: %lu console bytes dropped]\n", (unsigned long)st.lost);
            st.lost = 0;
            consoleSend(ci, data, len);
            return;
        }
    }
    if (len == 0) return;
    int sent = ::send(slot.fd(), data, len, MSG_DONTWAIT);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) { slot.stop(); return; }
        sent = 0;
    }
    if ((size_t)sent < len) {   // keep the rest for the next pass
        size_t rest = len - sent;
        if (rest > sizeof st.pend) { st.lost += rest - sizeof st.pend; rest = sizeof st.pend; }
        memcpy(st.pend, data + sent, rest);
        st.pendLen = rest;
    }
}

// connected() misreads a clean close as still connected while nothing is
// being sent; a peek tells the truth.
static bool peerGone(WiFiClient& c) {
    char dummy;
    int r = recv(c.fd(), &dummy, 1, MSG_DONTWAIT | MSG_PEEK);
    return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

// With nobody listening the stream is thrown away as it arrives, so the
// first client sees live output and not hours of backlog with a drop marker.
static void discardRaw() {
    uint8_t junk[256];
    while (xStreamBufferReceive(rawBuf, junk, sizeof junk, 0) > 0) {}
    rawDropped.exchange(0);
}

static void pumpConsole() {
    if (!rawBuf) return;
    if (!servicesUp) { discardRaw(); return; }
    for (auto& c : clients) if (c && c.connected() && peerGone(c)) c.stop();
    if (console.hasClient()) {
        WiFiClient c = console.accept();
        bool placed = false;
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
            WiFiClient& slot = clients[ci];
            if (slot && slot.connected()) continue;
            slot = c;
            slot.setNoDelay(true);
            // A peer that vanishes without a FIN still looks connected; keepalive finds out.
            int one = 1, idle = 45, interval = 15, count = 3;
            slot.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
            slot.setOption(TCP_KEEPIDLE, &idle);
            slot.setOption(TCP_KEEPINTVL, &interval);
            slot.setOption(TCP_KEEPCNT, &count);
            cstate[ci] = ConsoleState();
            slot.printf("%s console. MBB %s. Enter twice for the prompt.\n", nodeName,
                        mbbAwake() ? "awake" : "asleep, input dropped until it wakes");
            placed = true;
            break;
        }
        if (!placed) { c.print("all console slots are in use\n"); c.stop(); }
    }
    if (consoleClientCount() == 0) { discardRaw(); return; }
    // Output: an upstream drop marker if any, backlogs, then whatever the MBB said.
    uint32_t d = rawDropped.exchange(0);
    if (d) {
        char msg[64];
        int n = snprintf(msg, sizeof msg, "\n[dongle: %lu console bytes dropped upstream]\n", (unsigned long)d);
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++)
            if (clients[ci] && clients[ci].connected()) consoleSend(ci, (const uint8_t*)msg, n);
    }
    for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
        if (clients[ci] && clients[ci].connected() && cstate[ci].pendLen) consoleSend(ci, nullptr, 0);
    }
    uint8_t buf[512];
    size_t n;
    while ((n = xStreamBufferReceive(rawBuf, buf, sizeof buf, 0)) > 0) {
        for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
            if (clients[ci] && clients[ci].connected()) consoleSend(ci, buf, n);
        }
    }
    // Input: one client per pass, starting after the one served last time.
    for (size_t k = 0; k < CONSOLE_CLIENTS; k++) {
        size_t ci = (inputCursor + k) % CONSOLE_CLIENTS;
        WiFiClient& slot = clients[ci];
        if (!slot || !slot.connected() || !slot.available()) continue;
        uint8_t in[128], out[256];   // every byte can become two; sizes must keep that ratio
        int got = slot.read(in, sizeof in);
        size_t w = 0;
        uint8_t prev = cstate[ci].prev;
        for (int i = 0; i < got && w + 2 <= sizeof out; i++) {
            uint8_t b = in[i];
            if (b == 0x7f) b = 0x08;                            // delete to backspace
            if (b == '\n' && prev != '\r') out[w++] = '\r';     // the MBB wants CR LF
            out[w++] = b;
            prev = b;
        }
        cstate[ci].prev = prev;
        if (mbbAwake()) {
            size_t sent = mbbWrite(out, w);
            if (sent < w) slot.printf("(dongle: %u byte(s) of input not sent)\n", (unsigned)(w - sent));
        } else if (millis() - cstate[ci].asleepNoteMs > 1000) {
            cstate[ci].asleepNoteMs = millis();
            slot.print("(MBB asleep, input dropped)\n");
        }
        inputCursor = ci + 1;
        break;
    }
}

void netTick() {
    wm.process();
    uint32_t joins = ipEvents.load();
    if (joins != ipEventsSeen) {
        ipEventsSeen = joins;
        clockNetworkUp();   // on every join, so NTP is not left on a backoff
        startServices();
        Serial.printf("net: connected to %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !servicesUp) startServices();
    if (!connected) {
        // A wrong password shows as repeated authentication failures; a network
        // that is out of reach does not raise anything, however long it lasts.
        if (authFailures >= AUTH_FAILS_FOR_PORTAL) startPortal("authentication failed repeatedly");
        // With no setup network up, retry the saved network every 30 s. With one up
        // the user is in charge and WiFiManager connects when they save.
        if (millis() - lastRetryMs > 30000 && !wm.getConfigPortalActive() && wm.getWiFiIsSaved()) {
            lastRetryMs = millis();
            Serial.println("net: retrying saved WiFi");
            WiFi.begin();
        }
    }
    if (servicesUp) http.handleClient();
    pumpConsole();
}
