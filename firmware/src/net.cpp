// WiFi with a setup portal, mDNS, OTA, the HTTP server for the log files,
// and a raw TCP console onto the MBB.
#include "net.h"
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"

static WiFiManager wm;
static WebServer http(HTTP_PORT);
static WiFiServer console(CONSOLE_PORT);
static WiFiClient clients[CONSOLE_CLIENTS];
static StreamBufferHandle_t rawBuf;
static bool servicesStarted = false;
static uint32_t lastAsleepNoteMs = 0;
static uint8_t prevByte[CONSOLE_CLIENTS];
static uint32_t lastConnectedMs = 0;
static uint32_t lastRetryMs = 0;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>zero-dongle</title>
<style>body{font:14px system-ui,sans-serif;margin:1em;max-width:60em}pre{background:#f4f4f4;padding:.5em;overflow-x:auto;font-size:12px}table{border-collapse:collapse}td{padding:.1em .8em .1em 0}a{margin-right:1em}</style>
<h2>zero-dongle</h2><table id=s></table>
<h3>Files</h3><div id=f></div>
<h3>Last lines</h3><pre id=l></pre>
<p><a href=/update>Firmware update</a></p>
<script>
async function j(u){return (await fetch(u)).json()}
async function refresh(){
 const s=await j('/api/status');
 document.getElementById('s').innerHTML=Object.entries(s).map(([k,v])=>`<tr><td>${k}</td><td>${typeof v=='object'?JSON.stringify(v):v}</td></tr>`).join('');
 const f=await j('/logs');
 document.getElementById('f').innerHTML=f.map(x=>`<div><a href="/logs/${x.name}">${x.name}</a>${x.size} bytes${x.active?' (active)':''}</div>`).join('')||'none';
 document.getElementById('l').textContent=await (await fetch('/live')).text();
}
refresh();setInterval(refresh,5000);
</script>)HTML";

static const char UPDATE_FORM[] PROGMEM =
    "<form method=POST action=/update enctype=multipart/form-data>"
    "<input type=file name=firmware accept=.bin> <input type=submit value=Flash></form>";

static String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

static String statusJson() {
    size_t total, used;
    storeStats(total, used);
    String s = "{";
    s += "\"name\":\"" DONGLE_NAME "\",\"fw\":\"" FW_VERSION "\"";
    s += ",\"uptime_s\":" + String(millis() / 1000);
    s += ",\"boot\":" + String(storeBootCount());
    s += ",\"mbb_awake\":" + String(mbbAwake() ? "true" : "false");
    s += ",\"tx_attached\":" + String(mbbTxAttached() ? "true" : "false");
    s += ",\"time\":\"" + clockStamp() + "\",\"time_source\":\"" + clockSourceName() + "\"";
    s += ",\"wifi\":{\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",\"rssi\":" + String(WiFi.RSSI()) +
         ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    s += ",\"fs\":{\"total\":" + String(total) + ",\"used\":" + String(used) + "}";
    s += ",\"active\":\"" + storeActiveName() + "\"";
    s += ",\"dropped_lines\":" + String(storeDroppedLines());
    s += ",\"heap_free\":" + String(ESP.getFreeHeap());
    s += "}";
    return s;
}

static void handleFile() {
    String uri = http.uri();
    if (!uri.startsWith("/logs/")) {
        http.send(404, "text/plain", "not found");
        return;
    }
    String name = uri.substring(6);
    if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        http.send(400, "text/plain", "bad name");
        return;
    }
    if (http.method() == HTTP_GET) {
        if (name == storeActiveName()) {
            http.send(409, "text/plain", "file is active; see /live");
            return;
        }
        File f = storeOpenRead(name);
        if (!f) {
            http.send(404, "text/plain", "no such file");
            return;
        }
        http.streamFile(f, "text/plain");
        f.close();
        return;
    }
    if (http.method() == HTTP_DELETE) {
        if (name == storeActiveName()) {
            http.send(409, "text/plain", "file is active");
            return;
        }
        http.send(storeDelete(name) ? 200 : 404, "text/plain", "ok");
        return;
    }
    http.send(405, "text/plain", "method");
}

static void setupHttp() {
    http.on("/", HTTP_GET, []() { http.send_P(200, "text/html", PAGE); });
    http.on("/api/status", HTTP_GET, []() { http.send(200, "application/json", statusJson()); });
    http.on("/logs", HTTP_GET, []() { http.send(200, "application/json", storeListJson()); });
    http.on("/live", HTTP_GET, []() { http.send(200, "text/plain", storeLastLines()); });
    http.on("/api/wifi/reset", HTTP_POST, []() {
        http.send(200, "text/plain", "credentials cleared, rebooting into setup");
        delay(300);
        wm.resetSettings();
        ESP.restart();
    });
    http.on("/update", HTTP_GET, []() { http.send_P(200, "text/html", UPDATE_FORM); });
    http.on("/update", HTTP_POST,
        []() {
            http.sendHeader("Connection", "close");
            http.send(200, "text/plain", Update.hasError() ? "update failed" : "ok, rebooting");
            delay(300);
            if (!Update.hasError()) {
                storeSessionClose();   // end the file cleanly rather than mid-line
                ESP.restart();
            }
        },
        []() {
            HTTPUpload& up = http.upload();
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
    http.onNotFound(handleFile);
    http.begin();
}

static void startServices() {
    if (servicesStarted) return;   // mDNS and OTA survive a reconnect
    servicesStarted = true;
    Serial.printf("net: connected to %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    MDNS.begin(DONGLE_NAME);
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addService("zero-console", "tcp", CONSOLE_PORT);
    ArduinoOTA.setHostname(DONGLE_NAME);
    ArduinoOTA.begin();
}

void netBegin() {
    rawBuf = xStreamBufferCreate(8192, 1);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DONGLE_NAME);
    WiFi.setAutoReconnect(true);
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(20);
    wm.setHostname(DONGLE_NAME);
    if (wm.autoConnect(DONGLE_NAME, SETUP_AP_PASS)) {
        startServices();
    } else {
        Serial.println("net: no WiFi yet; setup AP " DONGLE_NAME " is up");
    }
    setupHttp();
    console.begin();
    console.setNoDelay(true);
}

void netPushRaw(const uint8_t* data, size_t len) {
    if (!rawBuf) return;
    uint8_t tmp[256];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0) continue;
        tmp[k++] = data[i];
        if (k == sizeof tmp) { xStreamBufferSend(rawBuf, tmp, k, 0); k = 0; }
    }
    if (k) xStreamBufferSend(rawBuf, tmp, k, 0);
}

static void pumpConsole() {
    if (console.hasClient()) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        WiFiClient c = console.accept();
#else
        WiFiClient c = console.available();
#endif
        bool placed = false;
        for (auto& slot : clients) {
            if (!slot || !slot.connected()) {
                slot = c;
                slot.setNoDelay(true);
                slot.printf("zero-dongle console. MBB %s. Enter twice for the prompt.\n",
                            mbbAwake() ? "awake" : "asleep, input dropped until it wakes");
                placed = true;
                break;
            }
        }
        if (!placed) c.stop();
    }
    uint8_t buf[512];
    size_t n;
    while ((n = xStreamBufferReceive(rawBuf, buf, sizeof buf, 0)) > 0) {
        for (auto& slot : clients) {
            if (!slot || !slot.connected()) continue;
            // Non-blocking: a client that cannot take the bytes loses them
            // rather than stalling the loop for seconds per chunk.
            int sent = ::send(slot.fd(), buf, n, MSG_DONTWAIT);
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) slot.stop();
        }
    }
    for (size_t ci = 0; ci < CONSOLE_CLIENTS; ci++) {
        WiFiClient& slot = clients[ci];
        if (!slot || !slot.connected() || !slot.available()) continue;
        uint8_t in[128], out[256];
        int got = slot.read(in, sizeof in);
        size_t k = 0;
        uint8_t prev = prevByte[ci];
        for (int i = 0; i < got; i++) {
            uint8_t b = in[i];
            if (b == 0x7f) b = 0x08;                            // delete to backspace
            if (b == '\n' && prev != '\r') out[k++] = '\r';     // the MBB wants CR LF
            out[k++] = b;
            prev = b;
        }
        prevByte[ci] = prev;
        if (mbbAwake()) {
            mbbWrite(out, k);
        } else if (millis() - lastAsleepNoteMs > 1000) {
            lastAsleepNoteMs = millis();
            slot.print("(MBB asleep, input dropped)\n");
        }
        break;   // only the first client with input gets to talk
    }
}

void netTick() {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) {
        startServices();
        ArduinoOTA.handle();
        lastConnectedMs = millis();
    } else {
        // A failed join at boot leaves the setup AP up and nothing retrying the
        // saved network. Retry it ourselves every 30 s; the setup AP stays up.
        if (wm.getWiFiIsSaved() && millis() - lastConnectedMs > 30000 && millis() - lastRetryMs > 30000) {
            lastRetryMs = millis();
            Serial.println("net: retrying saved WiFi");
            WiFi.begin();
        }
    }
    http.handleClient();
    pumpConsole();
}
