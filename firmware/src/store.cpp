// One file per MBB session in LittleFS, named bBBBB-SSS-<time>.log so that
// names sort by creation whether or not the clock was known when the file
// opened; the oldest by that order is deleted when space runs low. Lines
// wait in RAM and reach the flash only while the MBB is quiet, because a
// flash erase holds the UART interrupt off long enough to overrun its FIFO.
// Every entry point runs on the loop task; the recursive mutex is kept so
// that stays true by construction rather than by inspection.
#include "store.h"
#include "config.h"
#include "clock.h"
#include "util.h"
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
static String sessionId;     // the first file's stem, carried through rolled parts
static int sessionPart = 0;
static uint32_t bootCount = 0;
static uint32_t formats = 0;
static bool ok = false;
static const char* bootReason = "";
static uint32_t lastRotateMs = 0;
static uint32_t lastReclaimMs = (uint32_t)0 - RECLAIM_GAP_MS - 1;   // the first failure may reclaim at once
static String pending;
static bool pendingHasMbb = false;
static uint32_t pendingSinceMs = 0;
static String pendingFirstStamp;
static uint32_t droppedLines = 0;
static bool lastQuiet = true;
static String lastLines[LAST_LINES];
static int lastHead = 0, lastCount = 0;
static String lastAwake, lastAsleep;
static uint32_t awakeCount = 0;
static String openForRead[4];   // names being streamed out; reclaim and delete leave them alone

struct Lock {   // a no-op if the mutex was never created, so a failed begin cannot assert later
    Lock() { if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
    ~Lock() { if (mtx) xSemaphoreGiveRecursive(mtx); }
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

static size_t freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

// Delete oldest first until the reserve is back. Returns true if anything went.
static bool ensureSpace() {
    if (freeBytes() >= FS_MIN_FREE) return false;
    bool freed = false;
    for (auto& e : listEntries()) {
        if (e.name == activeName || isOpenForRead(e.name)) continue;
        if (!LittleFS.remove(pathOf(e.name))) {
            Serial.printf("store: cannot delete %s\n", e.name.c_str());
            continue;
        }
        Serial.printf("store: deleted %s for space\n", e.name.c_str());
        freed = true;
        if (freeBytes() >= FS_MIN_FREE) return true;   // the filesystem counts in blocks; ask it, do not guess
    }
    if (!freed) Serial.println("store: nothing to delete; every file is active or being read");
    return freed;
}

bool storeBegin(const char* resetReason) {
    mtx = xSemaphoreCreateRecursiveMutex();
    if (!mtx) { Serial.println("store: no memory for the lock"); return false; }
    bootReason = resetReason;
    Preferences p;
    bool nvs = p.begin("dongle", false);
    bootCount = p.getUInt("boots", 0) + 1;
    if (!nvs || !p.putUInt("boots", bootCount)) Serial.println("store: boot counter not saved");
    formats = p.getUInt("formats", 0);
    if (!LittleFS.begin(false)) {
        // Do not lose the logs quietly: count every format and say so.
        Serial.printf("store: LittleFS mount failed, formatting (format %lu)\n", (unsigned long)formats + 1);
        ok = LittleFS.begin(true);
        if (ok) { formats++; p.putUInt("formats", formats); }
    } else {
        ok = true;
    }
    p.end();
    if (ok && !LittleFS.exists(LOG_DIR) && !LittleFS.mkdir(LOG_DIR)) {
        Serial.println("store: cannot create the log directory");
        ok = false;
    }
    if (!ok) {
        Serial.println("store: no filesystem; capture will count dropped lines");
        return false;
    }
    pending.reserve(PENDING_MAX + 1536);   // one allocation, not a thousand
    Serial.printf("store: boot %lu, %u of %u bytes used\n", (unsigned long)bootCount,
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
}

// Write all of buf, retrying the unwritten remainder once after reclaiming
// space, so a partial first attempt is never duplicated. On failure any
// partial text is closed off with a marker so it cannot splice into the
// next line. Returns the bytes that reached the file's buffer.
static size_t writeAll(const char* buf, size_t len) {
    size_t done = 0;
    for (int attempt = 0; attempt < 2 && done < len; attempt++) {
        done += active.write((const uint8_t*)buf + done, len - done);
        if (done < len && attempt == 0) {
            uint32_t now = millis();
            if (now - lastReclaimMs > RECLAIM_GAP_MS) {
                lastReclaimMs = now;
                if (!ensureSpace()) break;   // nothing changed, so the retry would fail the same way
            } else {
                break;
            }
        }
    }
    if (done < len && done > 0) {
        static const char marker[] = " [dongle: truncated, flash full]\n";
        done += active.write((const uint8_t*)marker, sizeof marker - 1);
    }
    activeBytes += done;
    return done;
}

static bool writeLine(const String& line) {
    String withNl = line + "\n";
    if (writeAll(withNl.c_str(), withNl.length()) == withNl.length()) return true;
    droppedLines++;
    if (droppedLines == 1 || droppedLines % 100 == 0)
        Serial.printf("store: %lu line(s) dropped, flash full\n", (unsigned long)droppedLines);
    return false;
}

static void sessionOpen() {
    if (active) return;
    if (freeBytes() < FS_MIN_FREE) ensureSpace();
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
    int s = seq + 1;
    // A boot count that failed to save repeats, and a repeated name would
    // append to an old file the puller may already hold; skip past any name in use.
    do {
        snprintf(b, sizeof b, "b%04lu-%03d-%s.log", (unsigned long)bootCount, s, when.c_str());
    } while (LittleFS.exists(pathOf(b)) && ++s < 1000);
    File f = LittleFS.open(pathOf(b), FILE_APPEND);
    if (!f) {
        Serial.printf("store: cannot open %s\n", b);
        return;   // the sequence number is not spent on a failure
    }
    seq = s;
    active = f;
    activeName = b;
    activeBytes = 0;
    if (sessionPart == 0) { sessionId = activeName.substring(0, 9); sessionPart = 1; }
    else sessionPart++;
    writeLine((pendingFirstStamp.length() ? pendingFirstStamp : clockStamp()) +
              " dongle: session start, id " + sessionId + ", part " + String(sessionPart) +
              ", boot " + String(bootCount) + " (" + bootReason + "), time " + clockSourceName() +
              ", fw " FW_VERSION);
    Serial.printf("store: session %s\n", activeName.c_str());
}

static void sessionClose(const char* why) {
    if (!active) return;
    writeLine(clockStamp() + " dongle: " + why);
    active.flush();
    if (active.size() < activeBytes) droppedLines++;   // the closing line did not make it
    active.close();
    Serial.printf("store: closed %s\n", activeName.c_str());
    activeName = "";
}

void storeSessionClose() {
    Lock l;
    commitPending();   // the MBB has been quiet for SLEEP_AFTER_MS, so this is a safe time
    sessionClose("session end");
    sessionId = "";
    sessionPart = 0;
}

static uint32_t countLines(const String& s) {
    uint32_t n = 0;
    for (size_t i = 0; i < s.length(); i++) if (s[i] == '\n') n++;
    return n;
}

static void commitPending() {
    if (pending.length() == 0) return;
    if (!ok) { droppedLines += countLines(pending); pending = ""; pendingHasMbb = false; return; }
    if (!pendingHasMbb && !active) return;   // dongle notes alone wait for a session to join
    if (!active) sessionOpen();
    if (!active) { droppedLines += countLines(pending); pending = ""; pendingHasMbb = false; return; }
    size_t done = writeAll(pending.c_str(), pending.length());
    // The stdio buffer accepts writes a full filesystem cannot keep; flush()
    // returns nothing, but the file's size afterwards tells the truth. The
    // bytes lost there are the tail of what was written, so the loss is
    // counted once, from the tail of the buffer that did not reach the flash.
    active.flush();
    size_t onDisk = active.size();
    size_t lostAtFlush = onDisk < activeBytes ? activeBytes - onDisk : 0;
    if (lostAtFlush) {
        Serial.printf("store: commit lost %u byte(s) at the flush, flash full\n", (unsigned)lostAtFlush);
        activeBytes = onDisk;
    }
    size_t kept = done > lostAtFlush ? done - lostAtFlush : 0;
    if (kept < pending.length()) droppedLines += countLines(pending.substring(kept));
    pending = "";
    pendingHasMbb = false;
    if (activeBytes >= SESSION_MAX_BYTES) sessionClose("session continues in the next part");
}

void storeAppend(const String& line, bool fromMbb) {
    Lock l;
    lastLines[lastHead] = line.length() > LAST_LINE_CHARS ? line.substring(0, LAST_LINE_CHARS) : line;
    lastHead = (lastHead + 1) % LAST_LINES;
    if (lastCount < LAST_LINES) lastCount++;
    if (pending.length() == 0) pendingSinceMs = millis();
    if (fromMbb && !pendingHasMbb) {   // the header takes the first MBB line's own stamp, not a waiting note's
        int sp = line.indexOf(' ');
        pendingFirstStamp = sp > 0 ? line.substring(0, sp) : clockStamp();
    }
    if (!pending.concat(line) || !pending.concat('\n')) { droppedLines++; return; }   // out of memory
    if (fromMbb) pendingHasMbb = true;
    // On the loop task, so a commit here is allowed; but only while the MBB is
    // quiet, unless the buffer has grown past the hard cap regardless.
    if (pending.length() >= PENDING_MAX && (lastQuiet || pending.length() >= PENDING_MAX * 2)) commitPending();
}

void storeTick(bool mbbQuiet) {
    Lock l;
    lastQuiet = mbbQuiet;
    uint32_t now = millis();
    if (pending.length() && (mbbQuiet || now - pendingSinceMs > MAX_PENDING_MS)) commitPending();
    if (!ok) return;   // nothing to reclaim on a filesystem that is not there
    if (now - lastRotateMs > 60000 && (mbbQuiet || freeBytes() < FS_MIN_FREE / 2)) {
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
        out += "{\"name\":\"" + jsonEscape(e.name) + "\",\"size\":" + String(e.size) +
               ",\"active\":" + (e.name == activeName ? "true" : "false") + "}";
    }
    out += "]";
    return out;
}

static bool nameOk(const String& name) {
    if (name.length() == 0 || name.length() > 64 || !isalnum((unsigned char)name[0])) return false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return name.indexOf("..") < 0;
}

StoreDeleteResult storeDelete(const String& name) {
    Lock l;
    if (!nameOk(name) || name == activeName || isOpenForRead(name)) return STORE_REFUSED;
    if (!LittleFS.exists(pathOf(name))) return STORE_NOT_FOUND;
    return LittleFS.remove(pathOf(name)) ? STORE_DELETED : STORE_REFUSED;
}

File storeOpenRead(const String& name, bool* busy) {
    Lock l;
    if (busy) *busy = false;
    if (!nameOk(name) || name == activeName) return File();
    String* slot = nullptr;
    for (auto& n : openForRead) if (n.length() == 0) { slot = &n; break; }
    if (!slot) { if (busy) *busy = true; return File(); }   // unprotected reads are not handed out
    File f = LittleFS.open(pathOf(name), FILE_READ);
    if (f) *slot = name;
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
    out.reserve(LAST_LINES * (LAST_LINE_CHARS + 1));
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
