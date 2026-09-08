// The HTTP owner: the status and command pages, the JSON, the log files
// streamed a chunk at a time, settings and firmware updates. One client at
// a time, on the loop task, on the home network only.
#include "http.h"
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include "store.h"
#include "util.h"
#include "poller.h"
#include "sleep.h"
#include "settings.h"
#include "wlan.h"
#include "console.h"
#include "sys.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

static WebServer http(HTTP_PORT);
static bool up = false;
static uint32_t lastHttpMs = 0;
static void touch() { lastHttpMs = millis(); }
bool httpBusy() { return millis() - lastHttpMs < 30000; }

void httpStart() { if (!up) { up = true; http.begin(); } }
void httpStop() { if (up) { up = false; http.stop(); } }
void httpTick() { if (up) http.handleClient(); }

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
let tick=0;
async function refresh(){
 const s=await j('/api/status'); document.getElementById('t').textContent=s.name;
 const t=document.getElementById('s'); t.textContent=''; for(const [k,v] of Object.entries(s)) row(t,k,v);
 const st=s.store; document.getElementById('fs').textContent=st.files+' file(s), '+(st.bytes/1024).toFixed(0)+' KB on flash of '+(st.fs_total/1024).toFixed(0)+' KB, compressing '+st.ratio+'x since boot'+(st.days_left>=0?', about '+st.days_left+' day(s) of space left at this rate':'');
 if(tick++%6) return;   // the file list every 30 s: a walk of the flash per fetch
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

static String statusJson() {   // a health check is not use: a watcher must not keep the dongle awake
    size_t total, used;
    storeStats(total, used);
    String s;
    s.reserve(900);
    s += "{\"name\":\"" + String(wifiName()) + "\",\"mac\":\"" + wifiMac() + "\",\"fw\":\"" FW_VERSION "\"";
    s += ",\"uptime_s\":" + String(millis() / 1000);
    s += ",\"boot\":" + String(storeBootCount());
    s += ",\"reset_reason\":\"" + String(sysResetReason()) + "\"";
    s += ",\"watchdog\":" + String(sysWatchdogArmed() ? "true" : "false");
    s += ",\"mbb_awake\":" + String(mbbAwake() ? "true" : "false");
    s += ",\"line_high\":" + String(mbbLineHigh() ? "true" : "false");
    s += ",\"tx_attached\":" + String(mbbTxAttached() ? "true" : "false");
    s += ",\"time\":\"" + jsonEscape(clockStamp()) + "\",\"time_source\":\"" + clockSourceName() + "\"";
    s += ",\"ntp_age_s\":" + String(clockNtpAgeS() == UINT32_MAX ? -1 : (long)clockNtpAgeS());
    s += ",\"wifi\":" + wifiStatusJson();
    s += ",\"fs\":{\"total\":" + String(total) + ",\"used\":" + String(used) + ",\"ok\":" +
         (storeOk() ? "true" : "false") + ",\"formats\":" + String(storeFormats()) + "}";
    s += ",\"active\":\"" + jsonEscape(storeActiveName()) + "\"";
    s += "," + storeEdges();
    s += ",\"dropped_lines\":" + String(storeDroppedLines());
    s += ",\"uart\":{\"ok\":" + String(mbbOk() ? "true" : "false") + ",\"overflows\":" + String(mbbOverflows()) +
         ",\"backpressure\":" + String(mbbBackpressure()) + ",\"frame_errors\":" + String(mbbFrameErrors()) +
         ",\"queue_drops\":" + String(mbbQueueDrops()) + "}";
    s += ",\"console\":" + consoleStatusJson();
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
    s += ",\"loop_max_ms\":" + sysLoopMaxJson();
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
        http.send(200, name.endsWith(".gz") ? "application/gzip" : name.endsWith(".z") ? "application/zlib" : "text/plain", "");
        WiFiClient c = http.client();
        uint8_t buf[1024];
        bool whole = true;
        while (f.available() && c.connected()) {
            size_t n = f.read(buf, sizeof buf);
            if (n == 0) { whole = false; break; }   // a bad block: do not spin on it
            if (c.write(buf, n) != n) { whole = false; break; }
            sysTickCapture();
            consoleTick();
        }
        if (!whole) c.stop();   // the promised length will not arrive; say so by closing
        f.close();
        storeReadDone(name);
        touch();   // a long transfer ends with the puller's next request on its way
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

void httpBegin() {
    http.on("/", HTTP_GET, []() { http.send_P(200, "text/html", PAGE); });
    http.on("/api/status", HTTP_GET, []() { http.send(200, "application/json", statusJson()); });
    http.on("/logs", HTTP_GET, []() {   // streamed one file at a time: the directory is never held in RAM
        touch();
        http.setContentLength(CONTENT_LENGTH_UNKNOWN);
        http.send(200, "application/json", "");
        http.sendContent("[");
        bool first = true;
        storeForEachFile([](void* ctx, const char* name, size_t size, bool active) {
            bool* f = (bool*)ctx;
            String e = String(*f ? "" : ",") + "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String(size) + ",\"active\":" + (active ? "true" : "false") + "}";
            *f = false;
            http.sendContent(e);
            sysFeedWatchdog();
        }, &first);
        http.sendContent("]");
        http.sendContent("");
        touch();
    });
    http.on("/live", HTTP_GET, []() { touch(); http.send(200, "text/plain", storeLastLines()); });
    http.on("/api/settings", HTTP_GET, []() {
        http.send(200, "application/json", settingsJson());
    });
    http.on("/api/settings", HTTP_POST, []() {   // form fields tz, ntp, setup_pass, sleep, poll; any subset
        if (!tokenOk()) return;
        settingsApply(http.arg("tz"), http.arg("ntp"), http.arg("setup_pass"), http.arg("sleep"), http.arg("poll"), http.arg("sleep_days"),
                      http.arg("sleep_grace"));
        http.send(200, "text/plain", "applied");
    });
    http.on("/cmd", HTTP_GET, []() { touch(); http.send_P(200, "text/html", CMD_PAGE); });
    http.on("/api/cmd", HTTP_GET, []() { touch(); http.send(200, "application/json", pollerListJson()); });
    http.on("/api/cmd/poll", HTTP_POST, []() {
        if (!tokenOk()) return;
        touch();
        if (!pollerRequest()) { http.send(409, "text/plain", "the MBB is hibernating; not requested"); return; }
        http.send(200, "text/plain", "poll requested");
    });
    http.on("/api/wifi/reset", HTTP_POST, []() {
        if (!tokenOk()) return;
        http.send(200, "text/plain", "credentials cleared, rebooting into setup");
        sysTickCapture();   // lines framed but not yet delivered
        storeShutdown();
        wifiResetCredentials();
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
