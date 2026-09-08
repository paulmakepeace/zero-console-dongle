// The poller owns the transmit pin for the length of a batch: the hold stays
// attached across every command so the detach's own NUL, which makes the MBB
// print a prompt, cannot land mid-batch and be mistaken for a command's end.
// Output comes back through the capture module's line queue; the prompt line
// closes a command, unsolicited lines pass through to the log, and the rest
// is the command's output.
#include "poller.h"
#include "config.h"
#include "mbb_uart.h"
#include "util.h"
#include "pure/mbb_parse.h"

static const char* const CMDS[] = {POLL_CMDS};
static const int NCMD = sizeof CMDS / sizeof CMDS[0];
static String outputs[NCMD];        // the last good output of each; a failed attempt does not replace it
static uint32_t outputAtMs[NCMD];
static bool outputOk[NCMD];         // the last attempt succeeded
static uint32_t failedAtMs[NCMD];
static bool mbbStopping = false;    // the MBB has announced its hibernation: no more commands
static uint32_t stoppingSinceMs = 0;
static uint32_t intervalS = POLL_INTERVAL_S;
static uint32_t lastPollMs = 0;
static bool requested = false;
static bool running = false;
static bool finishAfterThis = false;
static int cur = -1;
static uint32_t cmdStartedMs = 0;
static bool expectEcho = false;
static String buf;
static uint32_t awakeSinceMs = 0;
static bool wasAwake = false;
static long soc = -1;
static PackRow pack = {-1, -1, 0, -1, -1, -999, -999};
static bool havePack = false;
static char bikeState[16] = "";

void pollerBegin(uint32_t s) { intervalS = s; buf.reserve(POLL_MAX_BYTES + 128); }
void pollerSetInterval(uint32_t s) { intervalS = s; }
uint32_t pollerInterval() { return intervalS; }
bool pollerRequest() { if (mbbStopping) return false; requested = true; return true; }
bool pollerActive() { return running; }
long pollerSoc() { return soc; }
const char* pollerBikeState() { return bikeState; }
bool pollerPack(long& s, long& mv, long& ma, long& ah, long& hi, long& lo) {
    if (!havePack) return false;
    s = pack.soc; mv = pack.packMv; ma = pack.currentMa; ah = pack.capacityAh; hi = pack.tempHiC; lo = pack.tempLoC;
    return true;
}

static int indexOf(const char* name) {
    for (int i = 0; i < NCMD; i++) if (strcmp(CMDS[i], name) == 0) return i;
    return -1;
}

static void sendCurrent() {
    buf = "";
    expectEcho = true;
    cmdStartedMs = millis();
    String line = String(CMDS[cur]) + "\r\n";
    if (mbbWrite((const uint8_t*)line.c_str(), line.length()) != line.length()) {
        outputOk[cur] = false;
        failedAtMs[cur] = millis() ? millis() : 1;
        cmdStartedMs = millis() - POLL_TIMEOUT_MS;   // the timeout path closes this one and moves on
    }
}

static void finish() {
    running = false;
    cur = -1;
    mbbTxHold(false);
    lastPollMs = millis();
}

static void closeCurrent(bool ok) {
    if (cur < 0 || cur >= NCMD) { finish(); return; }
    outputOk[cur] = ok;
    if (ok) { outputs[cur] = buf; outputAtMs[cur] = millis() ? millis() : 1; }
    else failedAtMs[cur] = millis() ? millis() : 1;   // the last good output stays
    if (ok && strcmp(CMDS[cur], "bms") == 0) soc = parseSoc(buf.c_str(), buf.length());
    if (ok && (strcmp(CMDS[cur], "state") == 0 || strcmp(CMDS[cur], "status") == 0)) {
        char st[16];
        if (parseBikeState(buf.c_str(), buf.length(), st, sizeof st)) strlcpy(bikeState, st, sizeof bikeState);
        PackRow r;
        if (parsePackRow(buf.c_str(), buf.length(), r)) { pack = r; havePack = true; soc = r.soc; }
    }
    buf = "";
    cur++;
    if (cur >= NCMD || finishAfterThis) finish();
    else sendCurrent();
}

bool pollerConsumeLine(const char* line, size_t len) {
    static const char stopping[] = "MBB will hibernate";
    for (size_t i = 0; i + sizeof(stopping) - 1 <= len; i++)
        if (memcmp(line + i, stopping, sizeof(stopping) - 1) == 0) { mbbStopping = true; stoppingSinceMs = millis() ? millis() : 1; break; }
    if (!running || cur < 0 || cur >= NCMD) return false;
    if (isPrompt(line, len)) { closeCurrent(true); return true; }
    if (isUnsolicited(line, len)) return false;   // the log wants these whatever we are doing
    if (len >= 7 && memcmp(line, "dongle:", 7) == 0) return false;   // the dongle's own markers belong in the file
    if (expectEcho) {
        expectEcho = false;
        if (len == strlen(CMDS[cur]) && memcmp(line, CMDS[cur], len) == 0) return true;   // our own echo
    }
    if (buf.length() + len + 1 <= POLL_MAX_BYTES) { buf.concat(line, len); buf += '\n'; }
    else if (!buf.endsWith("[dongle: output truncated]\n")) buf += "[dongle: output truncated]\n";
    return true;
}

void pollerTick(bool mbbAwake, bool consoleBusy) {
    uint32_t now = millis();
    if (mbbAwake != wasAwake) { wasAwake = mbbAwake; if (mbbAwake) { awakeSinceMs = now; mbbStopping = false; } }
    // An announcement the MBB did not follow through on (a key-on inside the
    // countdown keeps it up) stops mattering after a minute.
    if (mbbStopping && mbbAwake && now - stoppingSinceMs > 60000) mbbStopping = false;
    if (running) {
        if (!mbbAwake || mbbStopping) { finishAfterThis = true; closeCurrent(false); return; }   // it will not answer; keep what we have, end the batch
        if (consoleBusy) finishAfterThis = true;   // the console has priority; stop after this command
        if (now - cmdStartedMs > POLL_TIMEOUT_MS) {
            if (buf.length()) buf += "[dongle: no prompt within the timeout]\n";
            else buf = "(no answer)\n";
            closeCurrent(false);
        }
        return;
    }
    if (!mbbAwake || consoleBusy || mbbStopping) return;
    if (!requested && now - awakeSinceMs < POLL_SETTLE_MS) return;   // the schedule lets the MBB finish booting; a request is the operator's call
    bool due = intervalS && (lastPollMs == 0 || now - lastPollMs >= intervalS * 1000UL);
    if (!requested && !due) return;
    requested = false;
    finishAfterThis = false;
    running = true;
    cur = 0;
    mbbTxHold(true);
    sendCurrent();
}

String pollerListJson() {
    String s = "[";
    for (int i = 0; i < NCMD; i++) {
        if (i) s += ",";
        s += "{\"name\":\"" + String(CMDS[i]) + "\",\"bytes\":" + String(outputs[i].length()) +
             ",\"age_s\":" + String(outputAtMs[i] ? (long)((millis() - outputAtMs[i]) / 1000) : -1) +
             ",\"ok\":" + (outputOk[i] ? "true" : "false") +
             ",\"failed_s\":" + String(failedAtMs[i] ? (long)((millis() - failedAtMs[i]) / 1000) : -1) + "}";
    }
    return s + "]";
}

const String* pollerOutput(const char* name) {
    int i = indexOf(name);
    return i < 0 || !outputAtMs[i] ? nullptr : &outputs[i];
}

uint32_t pollerOutputAgeS(const char* name) {
    int i = indexOf(name);
    return i < 0 || !outputAtMs[i] ? UINT32_MAX : (millis() - outputAtMs[i]) / 1000;
}
