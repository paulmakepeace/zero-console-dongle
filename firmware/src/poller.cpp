// The poller owns the transmit pin for the length of a batch: the hold stays
// attached across every command so the detach's own NUL, which makes the MBB
// print a prompt, cannot land mid-batch and be mistaken for a command's end.
// Output comes back through the capture module's line queue; the prompt line
// closes a command and the rest is the command's output, kept here for the
// API. The log gets every line as well, behind a "dongle: poll" line per
// batch, so a session's polls read as a console transcript. A poll command
// typed by a console client is kept the same way as its answer goes by, so
// the page is as fresh as the session without a command of the poller's own.
#include "poller.h"
#include "readings.h"
#include "config.h"
#include "mbb_uart.h"
#include "util.h"
#include "store.h"
#include "clock.h"
#include "pure/mbb_parse.h"
#include <LittleFS.h>

static const char* const CMDS[] = {POLL_CMDS};
static const int NCMD = sizeof CMDS / sizeof CMDS[0];
static int indexOf(const char* name) {
    for (int i = 0; i < NCMD; i++) if (strcmp(CMDS[i], name) == 0) return i;
    return -1;
}
static String outputs[NCMD];        // the last good output of each; a failed attempt does not replace it
static uint32_t outputAtMs[NCMD];   // when an output was taken this boot; 0 for none, or one loaded from flash
static long outputEpoch[NCMD];      // wall time of an output loaded from flash, 0 otherwise
static bool saveDue = false;        // an output taken since the last save
static bool outputOk[NCMD];         // the last attempt succeeded
static uint32_t failedAtMs[NCMD];
static bool mbbStopping = false;    // the MBB has announced its hibernation: no more commands
static uint32_t stoppingSinceMs = 0;
static uint32_t intervalS = POLL_INTERVAL_S;
static uint32_t lastPollMs = 0;
static bool requested = false;
static bool requestSkipsSettle = false;   // a request made while the MBB was awake; one carried across a wake waits out the boot
static uint32_t quietWaitMs = 0;          // when a due batch first found the line not yet quiet
static bool running = false;
static bool finishAfterThis = false;
static int cur = -1;
static uint32_t cmdStartedMs = 0;
static bool expectEcho = false;
static String buf;
static int watch = -1;              // slot of a poll command typed on the console whose answer is going by; -1 for none
static uint32_t watchStartedMs = 0;
static bool consoleOn = false;      // latched from the tick: an echo with no client is not a typed command
static uint32_t awakeSinceMs = 0;
static bool wasAwake = false;
static long soc = -1;
static PackRow pack = {-1, -1, 0, -1, -1, -999, -999};
static bool havePack = false;
static char bikeState[16] = "";

// The last batch outlives a reboot: written once per session at the asleep
// edge, a quiet moment, and read at boot, so the command page has the
// bike's last known state before the MBB's next wake.
static void saveOutputs() {
    if (!clockValid()) return;   // an output with no wall time is no use after a reboot; the flag stays, the next edge tries again
    saveDue = false;
    String path = String("/") + POLL_SAVE_NAME, tmp = path + ".new";
    File f = LittleFS.open(tmp, "w");
    if (!f) return;
    long now = (long)time(nullptr);
    for (int i = 0; i < NCMD; i++) {
        if (!outputAtMs[i] && !outputEpoch[i]) continue;
        long at = outputAtMs[i] ? now - (long)((millis() - outputAtMs[i]) / 1000) : outputEpoch[i];
        f.printf("%ld %u %s\n", at, (unsigned)outputs[i].length(), CMDS[i]);   // the name last: it may hold a space
        f.print(outputs[i]);
    }
    bool whole = f.getWriteError() == 0;
    f.close();
    // The old save is only replaced once the new one is whole: a flash that
    // fills partway through must not cost both.
    if (whole) { LittleFS.remove(path); LittleFS.rename(tmp, path); }
    else LittleFS.remove(tmp);
}

static void loadOutputs() {
    File f = LittleFS.open(String("/") + POLL_SAVE_NAME, "r");
    if (!f) return;
    while (f.available()) {
        String head = f.readStringUntil('\n');
        char name[16]; long at; unsigned len;
        if (sscanf(head.c_str(), "%ld %u %15[^\n]", &at, &len, name) != 3 || len > POLL_MAX_BYTES + 64) break;
        int i = indexOf(name);
        String body;
        body.reserve(len);
        while (body.length() < len && f.available()) {
            char chunk[257];
            size_t n = f.readBytes(chunk, min((size_t)256, (size_t)(len - body.length())));
            if (!n) break;
            body.concat(chunk, n);
        }
        if (i >= 0 && body.length() == len) {
            outputs[i] = body; outputEpoch[i] = at; outputOk[i] = true;
            readingsFeed(body.c_str(), body.length(), at);   // the page's figures survive the reboot with their age
        }
    }
    f.close();
}

void pollerBegin(uint32_t s) { intervalS = s; buf.reserve(POLL_MAX_BYTES + 128); loadOutputs(); }
void pollerSetInterval(uint32_t s) { intervalS = s; }
uint32_t pollerInterval() { return intervalS; }
bool pollerRequest() {
    if (mbbStopping) return false;
    if (running) return true;   // the batch in flight is the answer: a request repeated while it runs, an HTTP retry for one, does not queue another
    requested = true; requestSkipsSettle = wasAwake;
    return true;
}
bool pollerActive() { return running; }
long pollerSoc() { return soc; }
const char* pollerBikeState() { return bikeState; }
bool pollerPack(long& s, long& mv, long& ma, long& ah, long& hi, long& lo) {
    if (!havePack) return false;
    s = pack.soc; mv = pack.packMv; ma = pack.currentMa; ah = pack.capacityAh; hi = pack.tempHiC; lo = pack.tempLoC;
    return true;
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

static void finish(bool quiet) {
    running = false;
    cur = -1;
    mbbTxHold(false);
    lastPollMs = millis();
    storeTick(quiet);   // the batch just ended: commit now if the MBB is quiet, so the output buffer never fills mid-batch and forces an erase while it is talking
}

// buf holds command i's whole output: keep it for the API, and read from it
// what the status carries.
static void keep(int i) {
    outputs[i] = buf; outputAtMs[i] = millis() ? millis() : 1; outputEpoch[i] = 0; outputOk[i] = true; saveDue = true;
    if (strcmp(CMDS[i], "bms") == 0) soc = parseSoc(buf.c_str(), buf.length());
    if (strcmp(CMDS[i], "state") == 0 || strcmp(CMDS[i], "status") == 0) {
        char st[16];
        if (parseBikeState(buf.c_str(), buf.length(), st, sizeof st)) strlcpy(bikeState, st, sizeof bikeState);
        PackRow r;
        if (parsePackRow(buf.c_str(), buf.length(), r)) { pack = r; havePack = true; soc = r.soc; }
    }
}

static void append(const char* line, size_t len) {
    if (buf.length() + len + 1 <= POLL_MAX_BYTES) { buf.concat(line, len); buf += '\n'; }
    else if (!buf.endsWith("[dongle: output truncated]\n")) buf += "[dongle: output truncated]\n";
}

static void closeCurrent(bool ok) {
    bool quiet = ok || millis() - mbbLastByteMs() > IDLE_FLUSH_MS;   // closed on its prompt, or the line has gone quiet: a command closed on its timeout may still be printing
    if (cur < 0 || cur >= NCMD) { finish(quiet); return; }
    if (ok) keep(cur);
    else { outputOk[cur] = false; failedAtMs[cur] = millis() ? millis() : 1; }   // the last good output stays
    buf = "";
    cur++;
    if (cur >= NCMD || finishAfterThis) finish(quiet);
    else {
        // The MBB is waiting for the next command: the quiet moment to write
        // the lines so far, so a full output buffer never forces an erase
        // under an answer, where it holds the UART interrupt off longer than
        // the FIFO covers.
        storeTick(quiet);
        sendCurrent();
    }
}

// A console client's typed command, echoed by the MBB as a line of its own
// or on the prompt's line when typed soon after it, opens a watch on its
// answer. Output lines are indented, so only a bare name at the line's
// start, or one after the prompt, counts.
static void tryOpen(const char* line, size_t len) {
    if (!consoleOn || watch >= 0 || running) return;
    static const char p[] = "ZERO MBB>";
    if (len >= sizeof(p) - 1 && memcmp(line, p, sizeof(p) - 1) == 0) {
        line += sizeof(p) - 1; len -= sizeof(p) - 1;
        while (len && *line == ' ') { line++; len--; }
    }
    if (len == 0 || len >= 16 || *line == ' ') return;
    char name[16];
    memcpy(name, line, len); name[len] = 0;
    int i = indexOf(name);
    if (i >= 0) { watch = i; watchStartedMs = millis(); buf = ""; }
}

bool pollerConsumeLine(const char* line, size_t len) {
    static const char stopping[] = "MBB will hibernate";
    for (size_t i = 0; i + sizeof(stopping) - 1 <= len; i++)
        if (memcmp(line + i, stopping, sizeof(stopping) - 1) == 0) { mbbStopping = true; stoppingSinceMs = millis() ? millis() : 1; break; }
    if (!running) {
        // No batch: a poll command typed by a console client is kept from
        // its answer as the batch would keep it.
        if (watch < 0) { tryOpen(line, len); return false; }
        if (isPrompt(line, len)) { keep(watch); buf = ""; watch = -1; tryOpen(line, len); return true; }   // the next echo may share the prompt's line
        if (isUnsolicited(line, len) || (len >= 7 && memcmp(line, "dongle:", 7) == 0)) return false;
        append(line, len);
        return true;
    }
    if (cur < 0 || cur >= NCMD) return false;
    if (isPrompt(line, len)) {
        static const char p[] = "ZERO MBB>";
        const char* r = line + sizeof(p) - 1; size_t rl = len - (sizeof(p) - 1);
        while (rl && *r == ' ') { r++; rl--; }
        // The previous command's prompt, late after its timeout, carrying this
        // command's echo: whatever arrived before it was that command's tail,
        // not this one's answer, and this one's close is still to come.
        if (rl == strlen(CMDS[cur]) && memcmp(r, CMDS[cur], rl) == 0) { buf = ""; expectEcho = false; return true; }
        // A bare prompt in the moment after the send is from before the batch:
        // this command's close cannot precede its echo.
        if (rl == 0 && expectEcho && millis() - cmdStartedMs < 500) return true;
        closeCurrent(true);
        if (!running) tryOpen(line, len);   // a batch ending here for a client: the echo may share the line
        return true;
    }
    if (isUnsolicited(line, len)) return false;   // the log wants these whatever we are doing
    if (len >= 7 && memcmp(line, "dongle:", 7) == 0) return false;   // the dongle's own markers belong in the file
    if (expectEcho) {
        expectEcho = false;
        if (len == strlen(CMDS[cur]) && memcmp(line, CMDS[cur], len) == 0) return true;   // our own echo
    }
    append(line, len);
    return true;
}

void pollerTick(bool mbbAwake, bool consoleBusy) {
    uint32_t now = millis();
    consoleOn = consoleBusy;
    // A typed command the MBB never closed, or one it went to sleep under, is
    // let go on the batch's own timeout; the last good output stays.
    if (watch >= 0 && !running && (!mbbAwake || now - watchStartedMs > POLL_TIMEOUT_MS)) { watch = -1; buf = ""; }
    if (mbbAwake != wasAwake) {
        wasAwake = mbbAwake;
        if (mbbAwake) { awakeSinceMs = now; mbbStopping = false; requestSkipsSettle = false; }   // a request from before the wake waits for the boot to finish
        else if (saveDue) saveOutputs();   // the session's last state, at the quiet edge
    }
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
    if (watch >= 0) return;   // a typed command still being answered: its lines would land in the batch's first slot
    // The framer may still hold the MBB's last prompt: it flushes after
    // IDLE_FLUSH_MS on the capture task, and the line reaches this task a
    // pass later, so wait a little past that. A chattering MBB does not
    // starve the batch: the echo rule below covers a late prompt too.
    if (now - mbbLastByteMs() < IDLE_FLUSH_MS + 500) {
        if (!quietWaitMs) quietWaitMs = now;
        if (now - quietWaitMs < 10000) return;
    }
    quietWaitMs = 0;
    if (!(requested && requestSkipsSettle) && now - awakeSinceMs < POLL_SETTLE_MS) return;   // the schedule lets the MBB finish booting; a request made while awake is the operator's call
    bool due = intervalS && (lastPollMs == 0 || now - lastPollMs >= intervalS * 1000UL);
    if (!requested && !due) return;
    requested = false;
    finishAfterThis = false;
    running = true;
    cur = 0;
    storeAppend(clockStamp() + " dongle: poll", false);
    mbbTxHold(true);
    sendCurrent();
}

String pollerListJson() {
    String s = "[";
    for (int i = 0; i < NCMD; i++) {
        if (i) s += ",";
        s += "{\"name\":\"" + String(CMDS[i]) + "\",\"bytes\":" + String(outputs[i].length()) +
             ",\"age_s\":" + String(pollerOutputAgeS(CMDS[i])) +
             ",\"ok\":" + (outputOk[i] ? "true" : "false") +
             ",\"failed_s\":" + String(failedAtMs[i] ? (long)((millis() - failedAtMs[i]) / 1000) : -1) + "}";
    }
    return s + "]";
}

const String* pollerOutput(const char* name) {
    int i = indexOf(name);
    return i < 0 || (!outputAtMs[i] && !outputEpoch[i]) ? nullptr : &outputs[i];
}

// Seconds since the output was taken; -1 for none, -2 for one from before
// this boot whose age waits on the clock.
long pollerOutputAgeS(const char* name) {
    int i = indexOf(name);
    if (i < 0) return -1;
    if (outputAtMs[i]) return (long)((millis() - outputAtMs[i]) / 1000);
    if (outputEpoch[i]) return clockValid() ? (long)time(nullptr) - outputEpoch[i] : -2;
    return -1;
}
