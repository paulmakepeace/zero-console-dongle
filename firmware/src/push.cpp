// The push owner: one file per pass of the loop, PUT to the archive as the
// flash holds it, oldest first, deleted on a 200. A session's dictionary,
// named in its file name, goes first once per round.
#include "push.h"
#include "config.h"
#include "store.h"
#include "sys.h"
#include "util.h"
#include "pure/push_url.h"
#include "pure/names.h"
#include <WiFi.h>

static String url;
static PushUrl target;
static bool due = false;
static String sessionCursor;   // the last session handled this round; a rejected file waits for the next request
static uint32_t sentDict = 0;  // the dictionary the server has been sent this round
static uint32_t nextTryMs = 0;
static uint32_t failures = 0;
static uint32_t pushed = 0, pushedBytes = 0, rejected = 0;
static String lastNote;
static uint32_t lastMs = 0;

void pushBegin(const char* u) { pushSetUrl(String(u)); }

bool pushSetUrl(const String& u) {
    if (u.length() == 0) { url = ""; due = false; return true; }
    PushUrl t;
    if (u.length() > PUSH_URL_MAX || !parsePushUrl(u.c_str(), u.length(), t)) return false;
    url = u;
    target = t;
    failures = 0;
    pushRequest();
    return true;
}

void pushRequest() {   // a join, a session end or a new URL: whatever the flash holds goes, dictionary first
    if (url.length() == 0) return;
    due = true;
    sessionCursor = "";
    sentDict = 0;
    if (failures >= PUSH_MAX_FAILURES) failures = 0;
    nextTryMs = millis();
}

bool pushBusy() { return due && WiFi.status() == WL_CONNECTED; }   // off the network nothing can move, so nothing holds the board up

// The oldest session above the cursor; the walk holds the store's lock and only compares names.
struct Pick { const String* cursor; String name; };
static void pick(void* ctx, const char* n, size_t, bool active) {
    Pick* p = (Pick*)ctx;
    if (active || *p->cursor >= n) return;
    if (p->name.length() == 0 || p->name > n) p->name = n;
}

static String nextSession() {
    Pick p = {&sessionCursor, String()};
    storeForEachFile(pick, &p);
    return p.name;
}

// PUT one file. Returns the HTTP status; 0 for no connection, no answer or no free reader; -1 for a file that is not there.
static int put(String name, size_t& sent) {   // by value: the capture pumped below can change what a caller's reference names
    sent = 0;
    bool busy = false;
    File f = storeOpenRead(name, &busy);
    if (!f) return busy ? 0 : -1;
    WiFiClient c;
    int status = 0;
    if (c.connect(target.host, target.port, PUSH_CONNECT_MS)) {
        c.setTimeout(3000);
        String req = "PUT " + String(target.path) + "/" + String(sysNodeName()) + "/" + name + " HTTP/1.1\r\n"
                     "Host: " + String(target.host) + "\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: " + String(f.size()) + "\r\n"
                     "Connection: close\r\n\r\n";
        bool whole = c.print(req) == (size_t)req.length();
        uint8_t buf[1024];
        while (whole && f.available() && c.connected()) {
            size_t n = f.read(buf, sizeof buf);
            if (n == 0 || c.write(buf, n) != n) { whole = false; break; }   // a bad block, or the socket went: do not spin
            sent += n;
            sysPumpTransfer();
        }
        if (whole) {
            uint32_t t0 = millis();   // the server ingests before it answers
            while (c.connected() && !c.available() && millis() - t0 < PUSH_REPLY_MS) { delay(20); sysPumpTransfer(); }
            String line = c.readStringUntil('\n');   // "HTTP/1.1 200 OK"
            if (line.startsWith("HTTP/1.") && line.length() > 12) status = line.substring(9, 12).toInt();
        }
        c.stop();
    }
    f.close();
    storeReadDone(name);
    sysNetUntimed();
    return status;
}

static void backoff(int status, const String& name) {
    failures++;
    lastNote = (status ? String(status) + " " : String("no answer ")) + name;
    nextTryMs = millis() + PUSH_BACKOFF_MS * failures;
    if (failures >= PUSH_MAX_FAILURES) { due = false; lastNote += ", giving up until the next join"; }
}

void pushTick() {
    if (!due || WiFi.status() != WL_CONNECTED) return;
    if ((int32_t)(millis() - nextTryMs) < 0) return;
    String name = nextSession();
    if (name.length() == 0) { due = false; return; }
    size_t sent = 0;
    uint32_t id = logDictId(name.c_str(), name.length());
    if (id && id != sentDict) {   // the store keeps a dictionary while any session names it
        char d[24];
        snprintf(d, sizeof d, "dict-%08lx.txt", (unsigned long)id);
        int st = put(String(d), sent);
        lastMs = millis();
        if (st == 200) { failures = 0; pushed++; pushedBytes += sent; sentDict = id; lastNote = "200 " + String(d); return; }
        if (st == 0 || st >= 500) { backoff(st, String(d)); return; }
        sentDict = id;   // not there or not wanted: the session goes without it and the server says
    }
    int status = put(name, sent);
    lastMs = millis();
    if (status == 200) {
        failures = 0; pushed++; pushedBytes += sent; sessionCursor = name; lastNote = "200 " + name;
        if (storeDelete(name) != STORE_DELETED) lastNote += " (kept)";
        return;
    }
    if ((status >= 400 && status < 500) || status == -1) {   // not wanted, or not there: on to the next
        rejected++; sessionCursor = name; lastNote = (status == -1 ? String("missing ") : String(status) + " ") + name;
        return;
    }
    backoff(status, name);
}

String pushStatusJson() {
    return String("{\"url\":\"") + jsonEscape(url) + "\",\"due\":" + (due ? "true" : "false") +
           ",\"pushed\":" + String(pushed) + ",\"bytes\":" + String(pushedBytes) + ",\"rejected\":" + String(rejected) +
           ",\"failures\":" + String(failures) + ",\"last\":\"" + jsonEscape(lastNote) + "\",\"last_age_s\":" +
           String(lastMs ? (long)((millis() - lastMs) / 1000) : -1) + "}";
}
