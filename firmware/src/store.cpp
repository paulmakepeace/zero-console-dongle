// One file per MBB session in LittleFS, oldest deleted when space runs low.
// Called from the capture task and the network loop, so everything takes the
// mutex. A session that opens before the clock is set gets a placeholder name
// and is renamed once the first MBB stamp or NTP arrives.
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
static bool activeUnsynced = false;
static uint32_t activeOpenMs = 0;
static int seq = 0;
static uint32_t bootCount = 0;
static bool dirty = false;
static uint32_t lastRotateMs = 0;
// Lines wait here and reach the flash only while the MBB is quiet: a flash
// erase holds the UART interrupt off long enough to overrun its FIFO, and
// the interrupt cannot be moved into IRAM under the precompiled core.
static String pending;
static uint32_t pendingSinceMs = 0;
static String lastLines[LAST_LINES];
static int lastHead = 0, lastCount = 0;
static uint32_t droppedLines = 0;
static uint32_t lastReclaimMs = 0;
static uint32_t formats = 0;
static bool ok = false;
static String lastAwake, lastAsleep;
static uint32_t awakeCount = 0;

struct Lock {
    Lock() { xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mtx); }
};

static String pathOf(const String& name) { return String(LOG_DIR) + "/" + name; }
static void commitPending();

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

static String timeName(uint32_t openedMs) {
    time_t t = time(nullptr) - (time_t)((millis() - openedMs) / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[24];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return String(b);
}

static void ensureSpace() {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    if (total - used >= FS_MIN_FREE) return;
    for (auto& e : listEntries()) {   // oldest first; skip what cannot go
        if (e.name == activeName) continue;
        if (!LittleFS.remove(pathOf(e.name))) {
            Serial.printf("store: cannot delete %s\n", e.name.c_str());
            continue;
        }
        Serial.printf("store: deleted %s for space\n", e.name.c_str());
        used = used > e.size ? used - e.size : 0;
        if (total - used >= FS_MIN_FREE) return;
    }
}

bool storeBegin() {
    mtx = xSemaphoreCreateRecursiveMutex();
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
// space, so a partial first attempt is never duplicated.
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
    return done == len;
}

static void writeLine(const String& line) {
    if (writeAll(line.c_str(), line.length()) && writeAll("\n", 1)) return;
    droppedLines++;
    if (droppedLines == 1 || droppedLines % 100 == 0)
        Serial.printf("store: %lu line(s) dropped, flash full\n", (unsigned long)droppedLines);
}

void storeSessionOpen() {
    Lock l;
    if (active) return;
    ensureSpace();
    activeOpenMs = millis();
    seq++;
    if (clockValid()) {
        activeName = timeName(activeOpenMs) + ".log";
        activeUnsynced = false;
    } else {
        char b[40];
        snprintf(b, sizeof b, "0000-b%lu-%d-u%lu.log", (unsigned long)bootCount, seq,
                 (unsigned long)(activeOpenMs / 1000));
        activeName = b;
        activeUnsynced = true;
    }
    active = LittleFS.open(pathOf(activeName), FILE_APPEND);
    if (!active) {
        Serial.printf("store: cannot open %s\n", activeName.c_str());
        activeName = "";
        return;
    }
    writeLine(clockStamp() + " dongle: session start, boot " + String(bootCount) +
              ", time " + clockSourceName() + ", fw " FW_VERSION);
    Serial.printf("store: session %s\n", activeName.c_str());
}

static void renameIfSynced() {
    if (!active || !activeUnsynced || !clockValid()) return;
    String oldName = activeName;
    String newName = timeName(activeOpenMs) + ".log";
    writeLine(clockStamp() + " dongle: clock set from " + clockSourceName() + ", was " + oldName);
    active.close();
    if (LittleFS.rename(pathOf(oldName), pathOf(newName))) activeName = newName;
    else Serial.printf("store: rename %s failed, keeping the name\n", oldName.c_str());
    active = LittleFS.open(pathOf(activeName), FILE_APPEND);
    if (!active) {
        Serial.printf("store: reopen %s failed\n", activeName.c_str());
        activeName = "";   // the next line starts a new file
    }
    activeUnsynced = false;
}

void storeSessionClose() {
    Lock l;
    commitPending();   // the MBB has been quiet for SLEEP_AFTER_MS, so this is a safe time
    if (!active) return;
    writeLine(clockStamp() + " dongle: session end");
    active.close();
    Serial.printf("store: closed %s\n", activeName.c_str());
    activeName = "";
    dirty = false;
}

static void commitPending() {
    if (pending.length() == 0) return;
    if (!ok) { droppedLines += 1; pending = ""; return; }
    if (!active) storeSessionOpen();
    if (!active) { droppedLines += 1; pending = ""; return; }
    renameIfSynced();
    if (!writeAll(pending.c_str(), pending.length())) {
        droppedLines++;
        if (droppedLines == 1 || droppedLines % 100 == 0)
            Serial.printf("store: %lu write(s) lost, flash full\n", (unsigned long)droppedLines);
    }
    pending = "";
    active.flush();
    dirty = false;
}

void storeAppend(const String& line) {
    Lock l;
    lastLines[lastHead] = line;
    lastHead = (lastHead + 1) % LAST_LINES;
    if (lastCount < LAST_LINES) lastCount++;
    if (pending.length() == 0) pendingSinceMs = millis();
    pending += line;
    pending += '\n';
    if (pending.length() >= PENDING_MAX) commitPending();
}

void storeTick() {
    Lock l;
    uint32_t now = millis();
    bool quiet = now - mbbLastByteMs() > IDLE_COMMIT_MS;
    if (pending.length() && (quiet || now - pendingSinceMs > MAX_PENDING_MS)) commitPending();
    if (quiet) renameIfSynced();   // a quiet session still gets its real name once the clock is known
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

bool storeDelete(const String& name) {
    Lock l;
    if (name.length() == 0 || name.indexOf('/') >= 0 || name == activeName) return false;
    return LittleFS.remove(pathOf(name));
}

File storeOpenRead(const String& name) {
    Lock l;
    if (name.length() == 0 || name.indexOf('/') >= 0) return File();
    return LittleFS.open(pathOf(name), FILE_READ);
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
