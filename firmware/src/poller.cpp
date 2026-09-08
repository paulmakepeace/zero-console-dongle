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
static String outputs[NCMD];
static uint32_t outputAtMs[NCMD];
static bool outputOk[NCMD];
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
static char bikeState[16] = "";

void pollerBegin(uint32_t s) { intervalS = s; buf.reserve(POLL_MAX_BYTES + 128); }
void pollerSetInterval(uint32_t s) { intervalS = s; }
uint32_t pollerInterval() { return intervalS; }
void pollerRequest() { requested = true; }
bool pollerActive() { return running; }
long pollerSoc() { return soc; }
const char* pollerBikeState() { return bikeState; }

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
        outputs[cur] = "(not sent)";
        outputOk[cur] = false;
        outputAtMs[cur] = millis();
        cur++;   // fall through to the next on the next tick's timeout path
        cmdStartedMs = millis() - POLL_TIMEOUT_MS;
    }
}

static void finish() {
    running = false;
    cur = -1;
    mbbTxHold(false);
    lastPollMs = millis();
}

static void closeCurrent(bool ok) {
    outputs[cur] = buf;
    outputOk[cur] = ok;
    outputAtMs[cur] = millis();
    if (ok && strcmp(CMDS[cur], "bms") == 0) soc = parseSoc(buf.c_str(), buf.length());
    if (ok && (strcmp(CMDS[cur], "state") == 0 || strcmp(CMDS[cur], "status") == 0)) {
        char st[16];
        if (parseBikeState(buf.c_str(), buf.length(), st, sizeof st)) strlcpy(bikeState, st, sizeof bikeState);
    }
    buf = "";
    cur++;
    if (cur >= NCMD || finishAfterThis) finish();
    else sendCurrent();
}

bool pollerConsumeLine(const char* line, size_t len) {
    if (!running || cur < 0) return false;
    if (isPrompt(line, len)) { closeCurrent(true); return true; }
    if (isUnsolicited(line, len)) return false;   // the log wants these whatever we are doing
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
    if (mbbAwake != wasAwake) { wasAwake = mbbAwake; if (mbbAwake) awakeSinceMs = now; }
    if (running) {
        if (!mbbAwake) { closeCurrent(false); if (running) finish(); return; }
        if (consoleBusy) finishAfterThis = true;   // the console has priority; stop after this command
        if (now - cmdStartedMs > POLL_TIMEOUT_MS) {
            if (buf.length()) buf += "[dongle: no prompt within the timeout]\n";
            else buf = "(no answer)\n";
            closeCurrent(false);
        }
        return;
    }
    if (!mbbAwake || consoleBusy) return;
    if (now - awakeSinceMs < POLL_SETTLE_MS) return;   // let the MBB finish booting first
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
             ",\"ok\":" + (outputOk[i] ? "true" : "false") + "}";
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
