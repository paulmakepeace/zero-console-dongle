// The push owner: one file per pass of the loop, PUT to the archive as the
// flash holds it, the capture pumped between chunks the way a download is.
// Sessions go oldest first; a 200 deletes the file. A session the server
// cannot inflate for want of its dictionary comes back 409 naming the id, so
// the dictionary is pushed and the session retried once; a dictionary no
// longer on the flash means the session is left with the server, which keeps
// the raw either way. Anything else backs off and gives up after a few tries
// until the next join or session end asks again.
#include "push.h"
#include "config.h"
#include "store.h"
#include "console.h"
#include "poller.h"
#include "mbb_uart.h"
#include "sys.h"
#include "util.h"
#include "pure/push_url.h"
#include <WiFi.h>

static String url;
static PushUrl target;
static bool configured = false;
static bool due = false;
static String sessionCursor;       // the last session handled this round; a rejected file is not tried again until the next request
static String pendingDict;         // a dictionary to push before retrying the session below, "" for none
static String pendingSession;      // the session waiting on pendingDict
static String retried;             // the session last retried after its dictionary went: a second 409 on it is final
static uint32_t nextTryMs = 0;
static uint32_t failures = 0;
static uint32_t pushed = 0, pushedBytes = 0, rejected = 0;
static String lastNote;
static uint32_t lastMs = 0;

void pushBegin(const char* u) { pushSetUrl(String(u)); }
const char* pushUrl() { return url.c_str(); }

bool pushSetUrl(const String& u) {
    if (u.length() == 0) { url = ""; configured = false; due = false; return true; }
    PushUrl t;
    if (u.length() > PUSH_URL_MAX || !parsePushUrl(u.c_str(), u.length(), t)) return false;
    url = u;
    target = t;
    configured = true;
    failures = 0;
    nextTryMs = millis();
    due = true;          // a URL just set or loaded at boot: whatever the flash holds goes now
    sessionCursor = "";
    pendingDict = "";
    return true;
}

void pushRequest() {
    if (!configured) return;
    due = true;
    sessionCursor = "";
    pendingDict = "";
    if (failures >= PUSH_MAX_FAILURES) { failures = 0; nextTryMs = millis(); }
}

bool pushBusy() { return configured && due; }

// The oldest session above the cursor; dictionaries are pushed on demand, not
// walked, since the listing does not carry them. The walk holds the store's
// lock and only compares names.
struct Pick { const String* cursor; String name; };
static void pick(void* ctx, const char* n, size_t, bool active) {
    Pick* p = (Pick*)ctx;
    if (active || strncmp(n, "dict-", 5) == 0) return;
    if (*p->cursor >= n) return;
    if (p->name.length() == 0 || p->name > n) p->name = n;
}

static String nextSession() {
    Pick p = {&sessionCursor, String()};
    storeForEachFile(pick, &p);
    return p.name;
}

// Pump what a transfer must keep alive, as the download path does.
static void pump() {
    sysTickCapture();
    consoleTick();
    if (pollerActive()) pollerTick(mbbAwake(), consoleClients() > 0);
}

// PUT one file, dictionary or session. Returns the HTTP status, 0 for no
// connection or no answer, -1 for a file that could not be opened; reply
// takes the first line of the response body, where a 409 names the dictionary.
static int put(const String& name, size_t& sent, String& reply) {
    sent = 0;
    reply = "";
    bool busy = false;
    File f = storeOpenRead(name, &busy);
    if (!f) return -1;
    WiFiClient c;
    int status = 0;
    if (c.connect(target.host, target.port, PUSH_CONNECT_MS)) {
        c.setTimeout(3000);   // ms per read; the server answers a PUT in well under this
        String req = "PUT " + String(target.path) + "/" + String(sysNodeName()) + "/" + name + " HTTP/1.1\r\n"
                     "Host: " + String(target.host) + "\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: " + String(f.size()) + "\r\n"
                     "X-Dongle-Fw: " FW_VERSION "\r\n"
                     "Connection: close\r\n\r\n";
        bool whole = c.print(req) == (size_t)req.length();
        uint8_t buf[1024];
        while (whole && f.available() && c.connected()) {
            size_t n = f.read(buf, sizeof buf);
            if (n == 0 || c.write(buf, n) != n) { whole = false; break; }   // a bad block, or the socket went: do not spin
            sent += n;
            pump();
        }
        if (whole) {
            uint32_t t0 = millis();   // the server ingests before it answers
            while (c.connected() && !c.available() && millis() - t0 < PUSH_REPLY_MS) { delay(20); pump(); }
            String line = c.readStringUntil('\n');   // "HTTP/1.1 200 OK"
            if (line.startsWith("HTTP/1.") && line.length() > 12) status = line.substring(9, 12).toInt();
            if (status == 409) {   // the body says "need dict-XXXXXXXX.txt"; the peer closes right behind it, so drain what is buffered before trusting connected()
                uint32_t t1 = millis();
                while (millis() - t1 < PUSH_REPLY_MS && reply.length() < 512) {
                    while (c.available() && reply.length() < 512) reply += (char)c.read();
                    if (!c.connected() && !c.available()) break;
                    delay(10); pump();
                }
            }
        }
        c.stop();
    }
    f.close();
    storeReadDone(name);
    sysNetUntimed();
    return status;
}

// The dictionary a 409 body names: "dict-XXXXXXXX.txt", or "" if absent.
static String neededDict(const String& reply) {
    int i = reply.indexOf("dict-");
    if (i < 0 || i + 17 > (int)reply.length()) return "";
    String d = reply.substring(i, i + 17);
    return d.endsWith(".txt") ? d : "";
}

static void backoff(int status, const String& name) {
    failures++;
    lastNote = (status ? String(status) + " " : String("no answer ")) + name;
    nextTryMs = millis() + PUSH_BACKOFF_MS * failures;
    if (failures >= PUSH_MAX_FAILURES) { due = false; lastNote += ", giving up until the next join"; }
}

void pushTick() {
    if (!configured || !due) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if ((int32_t)(millis() - nextTryMs) < 0) return;
    size_t sent = 0;
    String reply;

    if (pendingDict.length()) {   // a session's dictionary, before the session is retried
        int st = put(pendingDict, sent, reply);
        lastMs = millis();
        if (st == 200) { pushed++; pushedBytes += sent; retried = pendingSession; lastNote = "200 " + pendingDict; pendingDict = ""; failures = 0; return; }
        if (st == -1 || (st >= 400 && st < 500)) {   // the dictionary is gone from the flash or refused: leave the session with the server
            rejected++; sessionCursor = pendingSession; lastNote = "no dict " + pendingSession; pendingDict = ""; return;
        }
        backoff(st, pendingDict);
        return;
    }

    String name = nextSession();
    if (name.length() == 0) { due = false; return; }
    int status = put(name, sent, reply);
    lastMs = millis();
    if (status == 200) {
        failures = 0; pushed++; pushedBytes += sent; sessionCursor = name; lastNote = "200 " + name;
        if (storeDelete(name) != STORE_DELETED) lastNote += " (kept)";
        return;
    }
    if (status == 409) {
        String d = neededDict(reply);
        if (d.length() && retried != name) { pendingDict = d; pendingSession = name; lastNote = "409 " + name; return; }
        rejected++; sessionCursor = name; lastNote = "no dict " + name;   // unnamed, or already retried and still refused
        return;
    }
    if ((status >= 400 && status < 500) || status == -1) {   // not wanted, or unreadable: on to the next
        rejected++; sessionCursor = name; lastNote = (status == -1 ? String("unreadable ") : String(status) + " ") + name;
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
