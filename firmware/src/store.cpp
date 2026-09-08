// One file per MBB session in LittleFS, named bBBBB-SS-<time>.log so that
// names sort by creation whether or not the clock was known when the file
// opened; the oldest by that order is deleted when space runs low. Lines
// wait in RAM and reach the flash only while the MBB is quiet, because a
// flash erase holds the UART interrupt off long enough to overrun its FIFO.
// Called from the capture task and the network loop, so everything takes
// the mutex.
#include "store.h"
#include "config.h"
#include "clock.h"
#include "mbb_uart.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

struct Entry { String name; size_t size; };

static SemaphoreHandle_t mtx;
static File active;
static String activeName;
static size_t activeBytes = 0;
static int seq = 0;
static uint32_t bootCount = 0;
static uint32_t formats = 0;
static bool ok = false;
static const char* bootReason = "";
static bool dirty = false;
static uint32_t lastRotateMs = 0, lastReclaimMs = 0;
static String pending;
static uint32_t pendingSinceMs = 0;
static String pendingFirstStamp;
static uint32_t droppedLines = 0;
static String lastLines[LAST_LINES];
static int lastHead = 0, lastCount = 0;
static String lastAwake, lastAsleep;
static uint32_t awakeCount = 0;
static String openForRead[4];   // names being streamed out; reclaim and delete leave them alone
static String continuedFrom;    // set when a session rolls at SESSION_MAX_BYTES

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mtx); }
};

static String pathOf(const String& name) { return String(LOG_DIR) + "/" + name; }
static void commitPending();

static bool isOpenForRead(const String& name) {
    for (auto& n : openForRead) if (n == name) return true;
    return false;
}

static std::vector<Entry> listEntries() {
    std::vector<Entry> out;
    File dir = LittleFS.open(LOG_DIR);
    if (!dir) return out;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        String n = f.name();
        int slash = n.lastIndexOf('/');
        if (slash >= 0) n = n.substring(slash + 1);
        out.push_back({n, (size_t)f.size()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

static void ensureSpace() {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    if (total - used >= FS_MIN_FREE) return;
    for (auto& e : listEntries()) {   // oldest first; skip what cannot go
        if (e.name == activeName || isOpenForRead(e.name)) continue;
        if (!LittleFS.remove(pathOf(e.name))) {
            Serial.printf("store: cannot delete %s\n", e.name.c_str());
            continue;
        }
        Serial.printf("store: deleted %s for space\n", e.name.c_str());
        used = used > e.size ? used - e.size : 0;
        if (total - used >= FS_MIN_FREE) return;
    }
}

bool storeBegin(const char* resetReason) {
    mtx = xSemaphoreCreateRecursiveMutex();
    bootReason = resetReason;
    Preferences p;
    p.begin("dongle", false);
    bootCount = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", bootCount);
    formats = p.getUInt("formats", 0);
    if (!LittleFS.begin(false)) {
        // Do not lose the logs quietly: count every format and say so.
        formats++;
        p.putUInt("formats", formats);
        Serial.printf("store: LittleFS mount failed, formatting (format %lu)\n", (unsigned long)formats);
        ok = LittleFS.begin(true);
    } else {
        ok = true;
    }
    p.end();
    if (!ok) {
        Serial.println("store: no filesystem; capture will count dropped lines");
        return false;
    }
    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
    Serial.printf("store: boot %lu, %u of %u bytes used\n", (unsigned long)bootCount,
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
}

// Write all of buf, retrying the unwritten remainder once after reclaiming
// space, so a partial first attempt is never duplicated. On failure the
// partial text is closed off with a marker so it cannot splice into the
// next line.
static bool writeAll(const char* buf, size_t len) {
    size_t done = 0;
    for (int attempt = 0; attempt < 2 && done < len; attempt++) {
        done += active.write((const uint8_t*)buf + done, len - done);
        dirty = true;
        if (done < len && attempt == 0) {
            uint32_t now = millis();
            if (now - lastReclaimMs > RECLAIM_GAP_MS) {
                lastReclaimMs = now;
                ensureSpace();
            }
        }
    }
    if (done < len && done > 0) {
        static const char marker[] = " [dongle: truncated, flash full]\n";
        active.write((const uint8_t*)marker, sizeof marker - 1);
    }
    activeBytes += done;
    return done == len;
}

static void writeLine(const String& line) {
    if (writeAll(line.c_str(), line.length()) && writeAll("\n", 1)) return;
    droppedLines++;
    if (droppedLines == 1 || droppedLines % 100 == 0)
        Serial.printf("store: %lu line(s) dropped, flash full\n", (unsigned long)droppedLines);
}

static void sessionOpen() {
    if (active) return;
    ensureSpace();
    seq++;
    String when;
    if (clockValid()) {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        char b[24];
        strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
        when = b;
    } else {
        when = "nosync";
    }
    char b[48];
    snprintf(b, sizeof b, "b%04lu-%02d-%s.log", (unsigned long)bootCount, seq, when.c_str());
    activeName = b;
    activeBytes = 0;
    active = LittleFS.open(pathOf(activeName), FILE_APPEND);
    if (!active) {
        Serial.printf("store: cannot open %s\n", activeName.c_str());
        activeName = "";
        return;
    }
    writeLine((pendingFirstStamp.length() ? pendingFirstStamp : clockStamp()) +
              " dongle: session start, boot " + String(bootCount) + " (" + bootReason + "), time " +
              clockSourceName() + ", fw " FW_VERSION);
    if (continuedFrom.length()) {
        writeLine(clockStamp() + " dongle: session continued from " + continuedFrom);
        continuedFrom = "";
    }
    Serial.printf("store: session %s\n", activeName.c_str());
}

static void sessionClose(const char* why) {
    if (!active) return;
    writeLine(clockStamp() + " dongle: " + why);
    active.close();
    Serial.printf("store: closed %s\n", activeName.c_str());
    activeName = "";
    dirty = false;
}

void storeSessionClose() {
    Lock l;
    commitPending();   // the MBB has been quiet for SLEEP_AFTER_MS, so this is a safe time
    sessionClose("session end");
}

static uint32_t countLines(const String& s) {
    uint32_t n = 0;
    for (size_t i = 0; i < s.length(); i++) if (s[i] == '\n') n++;
    return n;
}

static void commitPending() {
    if (pending.length() == 0) return;
    if (!ok) { droppedLines += countLines(pending); pending = ""; return; }
    if (!active) sessionOpen();
    if (!active) { droppedLines += countLines(pending); pending = ""; return; }
    if (!writeAll(pending.c_str(), pending.length())) droppedLines += countLines(pending);
    pending = "";
    active.flush();
    dirty = false;
    if (activeBytes >= SESSION_MAX_BYTES) {
        String from = activeName;
        sessionClose("session continues in the next file");
        continuedFrom = from;
    }
}

void storeAppend(const String& line) {
    Lock l;
    lastLines[lastHead] = line.length() > LAST_LINE_CHARS ? line.substring(0, LAST_LINE_CHARS) : line;
    lastHead = (lastHead + 1) % LAST_LINES;
    if (lastCount < LAST_LINES) lastCount++;
    if (pending.length() == 0) { pendingSinceMs = millis(); pendingFirstStamp = clockStamp(); }
    pending += line;
    pending += '\n';
    if (pending.length() >= PENDING_MAX) commitPending();
}

void storeTick() {
    Lock l;
    uint32_t now = millis();
    bool quiet = now - mbbLastByteMs() > IDLE_COMMIT_MS;
    if (pending.length() && (quiet || now - pendingSinceMs > MAX_PENDING_MS)) commitPending();
    if (quiet && active && dirty) { active.flush(); dirty = false; }
    if (quiet && now - lastRotateMs > 60000) {
        lastRotateMs = now;
        ensureSpace();
    }
}

String storeActiveName() {
    Lock l;
    return activeName;
}

String storeListJson() {
    Lock l;
    String out = "[";
    bool first = true;
    for (auto& e : listEntries()) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + e.name + "\",\"size\":" + String(e.size) +
               ",\"active\":" + (e.name == activeName ? "true" : "false") + "}";
    }
    out += "]";
    return out;
}

static bool nameOk(const String& name) {
    if (name.length() == 0 || name.length() > 64) return false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return name.indexOf("..") < 0;
}

bool storeDelete(const String& name) {
    Lock l;
    if (!nameOk(name) || name == activeName || isOpenForRead(name)) return false;
    return LittleFS.remove(pathOf(name));
}

File storeOpenRead(const String& name) {
    Lock l;
    if (!nameOk(name)) return File();
    File f = LittleFS.open(pathOf(name), FILE_READ);
    if (f) for (auto& n : openForRead) if (n.length() == 0) { n = name; break; }
    return f;
}

void storeReadDone(const String& name) {
    Lock l;
    for (auto& n : openForRead) if (n == name) n = "";
}

void storeStats(size_t& total, size_t& used) {
    Lock l;
    total = LittleFS.totalBytes();
    used = LittleFS.usedBytes();
}

String storeLastLines() {
    Lock l;
    String out;
    int start = (lastHead - lastCount + LAST_LINES) % LAST_LINES;
    for (int i = 0; i < lastCount; i++) {
        out += lastLines[(start + i) % LAST_LINES];
        out += "\n";
    }
    return out;
}

uint32_t storeBootCount() { return bootCount; }
uint32_t storeDroppedLines() { return droppedLines; }
bool storeOk() { return ok; }
uint32_t storeFormats() { return formats; }

void storeNoteEdge(bool awake) {
    Lock l;
    if (awake) { lastAwake = clockStamp(); awakeCount++; }
    else lastAsleep = clockStamp();
}

String storeEdges() {
    Lock l;
    return "\"last_awake\":\"" + lastAwake + "\",\"last_asleep\":\"" + lastAsleep +
           "\",\"awake_count\":" + String(awakeCount);
}
