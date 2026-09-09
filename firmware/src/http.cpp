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
#include "readings.h"
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
static uint32_t lastHttpMs = 0;      // starts at 0: a boot counts as use for the window, someone just powered or flashed the board
static uint32_t useMs = HTTP_USE_MS;
static bool otaBegan = false;   // an upload part actually arrived, so a finish means something
// Use is a page opened or an action taken: a download or a delete, a poll
// request, a settings change. A page's own refreshes of the status, the
// live view, the listing and the command outputs do not count, so a tab
// left open does not hold the dongle awake.
static void touch() { lastHttpMs = millis(); }
bool httpBusy() { return millis() - lastHttpMs < useMs; }
void httpSetUseMs(uint32_t ms) { useMs = ms; }
uint32_t httpUseMs() { return useMs; }

void httpStart() { if (!up) { up = true; http.begin(); } }
void httpStop() { if (up) { up = false; http.stop(); } }
void httpTick() { if (up) http.handleClient(); }

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,maximum-scale=1"><meta name=color-scheme content="light dark">
<title>zero-dongle</title>
<style>:root{color-scheme:light dark}html,body{margin:0;max-width:100%;overflow-x:hidden}body{font:14px system-ui,sans-serif;padding:1em;box-sizing:border-box;width:100%;background:Canvas;color:CanvasText}pre{background:rgba(127,127,127,.15);padding:.5em;overflow-x:auto;font-size:12px}table{border-collapse:collapse;width:100%;table-layout:fixed}td{padding:.1em .8em .1em 0;overflow-wrap:anywhere;word-break:break-all}td:first-child{width:9em;word-break:normal}body,p,div{overflow-wrap:anywhere;word-break:break-word}a{margin-right:1em}</style>
<h2 id=t>zero-dongle</h2><p><a href=/cmd>Command outputs</a></p>
<h3>Bike</h3><table id=b></table>
<h3>Dongle</h3><table id=s></table>
<h3>Files</h3><div id=fs></div><div id=f></div>
<h3>Last lines</h3><pre id=l></pre>
<h3>Firmware update</h3>
<input type=file id=fw accept=.bin> <button id=go>Flash</button> <span id=fwmsg></span>
<p><a href=/setup style="color:#b00">Reinitialize network</a></p>
<script>
async function j(u){return (await fetch(u)).json()}
function row(t,k,v){const tr=t.insertRow();tr.insertCell().textContent=k;tr.insertCell().textContent=/^u\d{6}\.\d{3}$/.test(v)?'before the clock was set, '+parseFloat(v.slice(1))+' s after boot':typeof v=='object'?JSON.stringify(v):v}
function when(a){return a==-2?'before this boot':a<0?'':a<120?a+' s ago':Math.round(a/60)+' min ago'}
const GROUPS={pack:'Pack',motor:'Motor',attitude:'Attitude',trip:'Trip','12v':'12 V',cell:'Cellular and GPS',charge:'Charging',faults:'Faults'};
async function bike(){
 const r=await j('/api/readings'); const t=document.getElementById('b'); t.textContent='';
 if(!r.length){row(t,'no readings yet','the MBB has not answered a poll since boot');return}
 let g='';
 for(const x of r){if(x.g!=g){g=x.g;const tr=t.insertRow();const c=tr.insertCell();c.colSpan=2;c.textContent=GROUPS[g]||g;c.style.fontWeight='bold';c.style.paddingTop='.5em'}
  const tr=t.insertRow();tr.insertCell().textContent=x.n.replace(/_/g,' ');tr.insertCell().textContent=x.v+(x.u?' '+x.u:'')+(x.age_s>=0||x.age_s==-2?'  ('+when(x.age_s)+')':'')}
}
let tick=0;
async function refresh(){
 const s=await j('/api/status'); document.getElementById('t').textContent=s.name;
 try{await bike()}catch(e){}
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

// The setup page: the network to join and the settings, on the setup
// network and the home network alike. Plain fields, one script to post
// them with the header a cross-site form cannot set.
static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,maximum-scale=1"><meta name=color-scheme content="light dark">
<title>zero-dongle setup</title>
<style>:root{color-scheme:light dark}html,body{margin:0;max-width:100%;overflow-x:hidden}body{font:16px system-ui,sans-serif;padding:1em;box-sizing:border-box;width:100%;background:Canvas;color:CanvasText}label{display:block;margin:.6em 0}input{width:100%;max-width:100%;padding:.3em;box-sizing:border-box;font-size:16px}button{margin-top:1em;padding:.4em 1em}body,p,label,summary,h2,a,span{overflow-wrap:anywhere;word-break:break-word}.spin{display:inline-block;width:.9em;height:.9em;border:2px solid #ccc;border-top-color:#333;border-radius:50%;vertical-align:middle;animation:s 1s linear infinite}@keyframes s{to{transform:rotate(360deg)}}</style>
<h2 id=t>zero-dongle setup</h2>
<form id=f>
<h3>Network</h3>
<label>WiFi network <input name=ssid autocapitalize=off autofocus enterkeyhint=next></label>
<label>WiFi password <input name=pass type=password enterkeyhint=go></label>
<details><summary>Advanced settings</summary>
<label>Timezone, POSIX form <input name=tz></label>
<label>NTP server <input name=ntp></label>
<label>Sleep between MBB sessions, 1 or 0 <input name=sleep></label>
<label>Sleep only after N days unattended, 0 for always <input name=sleep_days></label>
<label>Poll the MBB every N seconds, 0 for never <input name=poll></label>
</details>
<button id=a>Apply</button>
<p id=m></p>
</form>
<p id=w>Network: looking</p>
<script>
const R={2:'wrong password',3:'wrong password',4:'the network refused the association',15:'wrong password',201:'network not found',202:'wrong password',204:'wrong password',205:'connection failed'};
let busy=0,d0=0,dn=0,typing=0;
async function w(){const e=document.getElementById('w');try{const s=await (await fetch('/api/status')).json();const n=s.wifi;const up=n.ip&&n.ip!='0.0.0.0';dn=n.disconnects;
 if(up||(busy&&n.disconnects>d0)){busy=0;document.getElementById('a').disabled=false}
 e.textContent='';
 if(up){e.textContent='Network: joined '+n.ssid+' as '+n.ip+'. The board is at ';const a=document.createElement('a');a.href='http://'+s.name+'.local/';a.target='_blank';a.textContent=s.name+'.local';e.appendChild(a);e.appendChild(document.createTextNode('; open it in Safari once this sheet closes, in about twenty seconds.'))}
 else e.textContent='Network: '+(busy?'joining':n.last_reason&&!typing?'not joined, last attempt: '+(R[n.last_reason]||'reason '+n.last_reason):'not joined yet');
 if(busy){e.appendChild(document.createTextNode(' '));const sp=document.createElement('span');sp.className='spin';e.appendChild(sp)}
 }catch(x){e.textContent='Network: no answer; rejoin '+location.hostname+' if the phone dropped it'}}
w();setInterval(w,3000);
fetch('/api/settings').then(r=>r.json()).then(s=>{for(const k of ['tz','ntp','sleep','sleep_days','poll']) document.querySelector('[name='+k+']').value=s[k]}).catch(()=>{});
fetch('/api/status').then(r=>r.json()).then(s=>{document.getElementById('t').textContent=s.name+' setup'}).catch(()=>{});
const F=document.getElementById('f');
F.elements.ssid.addEventListener('keydown',e=>{if(e.key=='Enter'){e.preventDefault();F.elements.pass.focus()}});
for(const k of ['ssid','pass']) F.elements[k].addEventListener('focus',()=>{typing=1;w()});
F.onsubmit=async e=>{e.preventDefault();
 const b=new URLSearchParams(new FormData(e.target)); document.getElementById('m').textContent='Applying'; busy=!!b.get('ssid'); typing=0; d0=dn; document.getElementById('a').disabled=true;
 const r=await fetch('/setup',{method:'POST',headers:{'X-Dongle':'1','Content-Type':'application/x-www-form-urlencoded'},body:b});
 document.getElementById('m').textContent=await r.text();};
</script>)HTML";

static const char CMD_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,maximum-scale=1"><meta name=color-scheme content="light dark">
<title>zero-dongle commands</title>
<style>:root{color-scheme:light dark}html,body{margin:0;max-width:100%;overflow-x:hidden}body{font:14px system-ui,sans-serif;padding:1em;box-sizing:border-box;width:100%;background:Canvas;color:CanvasText}pre{background:rgba(127,127,127,.15);padding:.5em;overflow-x:auto;font-size:12px;min-height:10em}button{margin:0 .3em .5em 0;padding:.3em .8em}button.on{font-weight:bold;background:rgba(127,127,127,.3)}#age{color:#888;overflow-wrap:anywhere}body{overflow-wrap:anywhere;word-break:break-word}.spin{display:inline-block;width:.9em;height:.9em;border:2px solid #ccc;border-top-color:#333;border-radius:50%;vertical-align:middle;animation:s 1s linear infinite}@keyframes s{to{transform:rotate(360deg)}}</style>
<p><a href=/>&#9664; Back to main</a></p>
<div id=tabs></div>
<div id=age></div>
<pre id=out>loading</pre>
<script>
let cur=null,list=[],want=0;
const out=document.getElementById('out'),age=document.getElementById('age');
function when(a){return a==-2?'from before this boot':a<0?'':a+' s ago'}
async function tabs(){
 list=await (await fetch('/api/cmd')).json();
 const t=document.getElementById('tabs'); t.textContent='';
 for(const c of list){const b=document.createElement('button'); b.textContent=c.name; b.className=c.name==cur?'on':''; b.onclick=()=>pick(c.name); t.appendChild(b)}
 if(!cur&&list.length) cur=list[0].name;
}
function spin(){out.textContent='';const s=document.createElement('span');s.className='spin';out.appendChild(s)}
let note='';
async function show(){
 if(!cur) return;
 for(const b of document.querySelectorAll('#tabs button')) b.className=b.textContent==cur?'on':'';
 const r=await fetch('/api/cmd/'+encodeURIComponent(cur));
 const a=+r.headers.get('X-Age-Seconds');
 if(r.ok){out.textContent=await r.text();age.textContent=cur+(when(a)?', '+when(a):'')+note}
 else{age.textContent=cur+': '+(await r.text())+note;if(!want) out.textContent=''}
 return r.ok?a:-1;
}
async function pick(name){
 cur=name; note=''; const had=await show();
 const s=await (await fetch('/api/status')).json();
 if(!s.mbb_awake){note=' (the MBB is asleep; a fresh answer comes at its next wake)';await show();return}
 const p=await fetch('/api/cmd/poll',{method:'POST',headers:{'X-Dongle':'1'}});
 if(!p.ok){note=' ('+await p.text()+')';await show();return}
 want=1; if(had==-1) spin(); else {age.textContent=cur+', '+when(had)+' ';const sp=document.createElement('span');sp.className='spin';age.appendChild(sp)}
 const t0=Date.now();
 while(Date.now()-t0<40000){await new Promise(r=>setTimeout(r,2000));const now=await show();if(now>=0&&(had<0||now<had)){want=0;return}}
 want=0; note=' (no fresh answer yet)'; await show();
}
tabs().then(()=>pick(cur)); setInterval(()=>{if(!want) show()},15000);
</script>)HTML";

static String statusJson() {   // a health check is not use: a watcher must not keep the dongle awake
    size_t total, used;
    storeStats(total, used);
    String s;
    s.reserve(900);
    s += "{\"name\":\"" + String(sysNodeName()) + "\",\"mac\":\"" + wifiMac() + "\",\"fw\":\"" FW_VERSION "\"";
    s += ",\"uptime_s\":" + String(millis() / 1000);
    s += ",\"boot\":" + String(storeBootCount());
    s += ",\"reset_reason\":\"" + String(sysResetReason()) + "\"";
    s += ",\"watchdog\":" + String(sysWatchdogArmed() ? "true" : "false");
    s += ",\"mbb_awake\":" + String(mbbAwake() ? "true" : "false");
    s += ",\"line_high\":" + String(mbbLineHigh() ? "true" : "false");
    s += ",\"tx_attached\":" + String(mbbTxAttached() ? "true" : "false");
    s += ",\"wake_hold_s\":" + String(mbbWakeHoldS());
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
             ",\"age_s\":" + String(pollerOutputAgeS("status"));
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
    const String* out = pollerOutput(name.c_str());
    if (!out) {
        bool known = pollerHasCommand(name.c_str());   // exactly, so a prefix is not a command
        http.send(known ? 503 : 404, "text/plain", known ? "not polled yet" : "no such command");
        return;
    }
    http.sendHeader("X-Age-Seconds", String(pollerOutputAgeS(name.c_str())));   // -2: from before this boot, the clock not yet set
    http.send(200, "text/plain", *out);
}

static void handleFile() {
    String uri = http.uri();
    if (uri.startsWith("/api/cmd/")) { handleCmd(WebServer::urlDecode(uri.substring(9))); return; }   // the server leaves the path encoded: "bms%20interface"
    if (!uri.startsWith("/logs/")) {   // a stray probe is not use
        if (wifiOnSetupNetwork(http.client().localIP())) {   // a phone checking for the internet: this is a captive network, here is its page; its sheet closes when the setup network goes
            http.sendHeader("Cache-Control", "no-store");
            http.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
            http.send(302, "text/html", "<a href=\"/setup\">setup</a>");
            return;
        }
        http.send(404, "text/plain", "not found");
        return;
    }
    String name = uri.substring(6);
    if (name.length() == 0) {
        http.send(404, "text/plain", "not found");   // a stray probe is not use
        return;
    }
    if (http.method() == HTTP_GET) {
        touch();
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
            if (pollerActive()) pollerTick(mbbAwake(), consoleClients() > 0);   // a batch in flight ends; a transfer is no moment to start one
        }
        if (!whole) c.stop();   // the promised length will not arrive; say so by closing
        f.close();
        storeReadDone(name);
        touch();   // a long transfer ends with the puller's next request on its way
        sysNetUntimed();
        return;
    }
    if (http.method() == HTTP_DELETE) {
        if (!tokenOk()) return;
        touch();   // an action, like the download it usually follows
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
    http.on("/", HTTP_GET, []() { touch(); http.send_P(200, "text/html", PAGE); });
    http.on("/api/status", HTTP_GET, []() { http.send(200, "application/json", statusJson()); });
    http.on("/logs", HTTP_GET, []() {   // streamed one file at a time: the directory is never held in RAM
        http.setContentLength(CONTENT_LENGTH_UNKNOWN);
        http.send(200, "application/json", "");
        http.sendContent("[");
        sysNetUntimed();   // a listing to a slow client is not a stall
        bool first = true;
        storeForEachFile([](void* ctx, const char* name, size_t size, bool active) {
            bool* f = (bool*)ctx;
            String e = String(*f ? "" : ",") + "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String(size) + ",\"active\":" + (active ? "true" : "false") + "}";
            *f = false;
            http.sendContent(e);
            // Only the watchdog here: the walk holds the store's lock and is
            // reading the directory, so a capture tick, which can commit a
            // line to flash, must not run inside it. A client that stalls
            // mid-listing pauses the capture for the length of the stall,
            // bounded by the file count; the transfer loop below is the path
            // that needed pumping and has it.
            sysFeedWatchdog();
        }, &first);
        http.sendContent("]");
        http.sendContent("");
    });
    http.on("/live", HTTP_GET, []() { http.send(200, "text/plain", storeLastLines()); });
    http.on("/api/settings", HTTP_GET, []() {
        http.send(200, "application/json", settingsJson());
    });
    http.on("/api/settings", HTTP_POST, []() {   // form fields tz, ntp, sleep, poll, sleep_days, and the bench knobs sleep_grace and use_s; any subset
        if (!tokenOk()) return;
        touch();
        if (!settingsApply(http.arg("tz"), http.arg("ntp"), http.arg("sleep"), http.arg("poll"), http.arg("sleep_days"),
                           http.arg("sleep_grace"), http.arg("use_s"))) {
            http.send(400, "text/plain", "a value is too long: tz 63, ntp 64 characters at most");
            return;
        }
        http.send(200, "text/plain", "applied");
    });
    http.on("/cmd", HTTP_GET, []() { touch(); http.send_P(200, "text/html", CMD_PAGE); });
    http.on("/setup", HTTP_GET, []() { touch(); http.send_P(200, "text/html", SETUP_PAGE); });
    http.on("/setup", HTTP_POST, []() {   // the setup page's form: a network to join, if given, and the settings
        if (!tokenOk()) return;
        touch();
        if (!settingsApply(http.arg("tz"), http.arg("ntp"), http.arg("sleep"), http.arg("poll"), http.arg("sleep_days"))) {
            http.send(400, "text/plain", "a value is too long: tz 63, ntp 64 characters at most");
            return;
        }
        String ssid = http.arg("ssid");
        if (ssid.length() == 0) { http.send(200, "text/plain", "Settings applied."); return; }
        if (ssid.length() > 32 || http.arg("pass").length() > 64) { http.send(400, "text/plain", "network name 32 and password 64 characters at most"); return; }
        http.send(200, "text/plain", "Settings applied; joining " + ssid + ". The setup network goes down once joined; then find the board at " + String(sysNodeName()) + ".local.");
        delay(200);   // the reply out before the radio changes
        wifiJoin(ssid.c_str(), http.arg("pass").c_str());
    });
    http.on("/api/cmd", HTTP_GET, []() { http.send(200, "application/json", pollerListJson()); });
    http.on("/api/readings", HTTP_GET, []() { http.send(200, "application/json", readingsJson()); });   // a page's refresh, not use
    http.on("/api/wake", HTTP_POST, []() {   // pin 9 high for a hold: wakes a hibernating MBB and keeps it awake, for the CCM check-in experiment
        if (!tokenOk()) return;
        touch();
        // Only a hibernating MBB is woken. A reset landing on a wake that is
        // charging the 12 V battery abandons the top-up, which is the drain
        // this experiment exists to study.
        if (mbbAwake()) { http.send(409, "text/plain", "the MBB is already awake; nothing to wake"); return; }
        long hold = http.hasArg("hold") ? http.arg("hold").toInt() : 120;
        if (hold < 1) hold = 1;
        if (hold > 900) hold = 900;
        sleepNoteProvokedWake();   // the lines this session prints are not the bike being attended
        mbbWake((uint32_t)hold * 1000UL);
        http.send(200, "text/plain", "waking, held for " + String(hold) + " s");
    });
    http.on("/api/cmd/poll", HTTP_POST, []() {
        if (!tokenOk()) return;
        touch();
        if (!pollerRequest()) { http.send(409, "text/plain", "the MBB is hibernating; not requested"); return; }
        http.send(200, "text/plain", "poll requested");
    });
    http.on("/api/wifi/reset", HTTP_POST, []() {
        if (!tokenOk()) return;
        http.send(200, "text/plain", "clearing credentials, rebooting into setup");   // sent before the radio drops
        sysTickCapture();   // lines framed but not yet delivered
        storeShutdown();
        if (!wifiResetCredentials()) Serial.println("http: credentials not cleared; the board will rejoin, try again");
        delay(500);   // let the WiFi task commit the erase before the reset
        ESP.restart();
    });
    http.on("/update", HTTP_POST,
        []() {
            http.sendHeader("Connection", "close");
            if (http.header("X-Dongle") != "1") { http.send(403, "text/plain", "missing X-Dongle: 1 header"); return; }
            // A fresh Update reports finished, since nothing written is
            // nothing outstanding: only an upload that actually began can
            // have flashed anything.
            bool began = otaBegan;
            otaBegan = false;
            bool ok = began && Update.isFinished() && !Update.hasError();
            if (!began) { http.send(400, "text/plain", "no firmware part in the request"); return; }
            http.send(ok ? 200 : 500, "text/plain", ok ? "ok, rebooting" : "update failed");
            sysNetUntimed();
            delay(300);
            if (ok) {
                sysTickCapture();   // lines framed but not yet delivered
                storeShutdown();    // end the file cleanly rather than mid-line
                ESP.restart();
            }
        },
        []() {
            sysTickCapture();   // a slow link can take minutes; feed even while refusing
            if (pollerActive()) pollerTick(mbbAwake(), consoleClients() > 0);   // the timeout that ends a stuck batch lives here: without it the transmit pin stays attached for the whole upload
            sysNetUntimed();
            if (http.header("X-Dongle") != "1") return;
            // The server hands a body that is not multipart to this same
            // handler down its raw path, where there is no upload object at
            // all and reading one would fault.
            if (!http.header("Content-Type").startsWith("multipart/")) return;
            HTTPUpload& up = http.upload();
            if (up.status == UPLOAD_FILE_START) {
                Serial.printf("ota: %s\n", up.filename.c_str());
                otaBegan = true;
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
    const char* headers[] = {"X-Dongle", "Content-Type"};   // the content type tells an upload from a raw body
    http.collectHeaders(headers, 2);
    http.onNotFound(handleFile);
}
