// One gzip file per MBB session in LittleFS, named bBBBB-SSS-<time>.log.gz so
// that names sort by creation whether or not the clock was known when the
// session began; the oldest by that order is deleted when space runs low.
// Lines are compressed as they arrive, on fixed arrays, and the compressed
// bytes reach the flash only while the MBB is quiet, because a flash erase
// holds the UART interrupt off long enough to overrun its FIFO. Every entry
// point runs on the loop task; the recursive mutex keeps that true by
// construction rather than by inspection.
#include "store.h"
#include "config.h"
#include "clock.h"
#include "util.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include "esp_mac.h"
#include "pure/names.h"
#include "pure/gzstream.h"

struct Entry { String name; size_t size; };

static SemaphoreHandle_t mtx;
static File active;
static String activeName;        // set when a session is prepared; the file itself opens at the first commit
static size_t activeBytes = 0;
static int seq = 0;
static String sessionId;         // the first part's stem, carried through rolled parts
static int sessionPart = 0;
static uint32_t bootCount = 0;
static uint32_t formats = 0;
static bool ok = false;
static const char* bootReason = "";
static char boardName[24];       // the file name carries no board id; the header does
static uint32_t lastRotateMs = 0;
static uint32_t lastReclaimMs = (uint32_t)0 - RECLAIM_GAP_MS - 1;   // the first failure may reclaim at once
static uint32_t lastOpenFailMs = (uint32_t)0 - RECLAIM_GAP_MS - 1;
static uint32_t lastSpaceCheckMs = 0;
static GzStream<GZ_HISTORY, GZ_LINE_CAP, GZ_OUT, GZ_HASH_BITS> gz;   // fixed arrays, never on the heap
static bool streamOpen = false;  // gz is begun
static bool named = false;       // the session has its name and header
static bool endWanted = false;   // the session ended before its file could be created; close at the first commit
static bool pendingHasMbb = false;
static uint32_t pendingLines = 0;   // lines in gz's buffer since the last commit
static uint32_t pendingSinceMs = 0;
static uint32_t droppedLines = 0;
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
static void commitPending(bool force);
static void sessionEnd(const char* why);

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
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(boardName, sizeof boardName, "%s-%02x%02x", DONGLE_NAME, mac[4], mac[5]);
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
    Serial.printf("store: boot %lu, %u of %u bytes used\n", (unsigned long)bootCount,
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
}

// A session is a stream in RAM until it earns a file. The stream starts at
// the first line of any kind so that dongle notes can wait in it; the name
// and the header are settled at the first MBB line, when the clock and the
// session are real; the file itself is created at the first commit, so the
// flash is not touched while the MBB is talking.
static void streamStart() {
    if (streamOpen) return;
    gz.begin();
    streamOpen = true;
    named = false;
    endWanted = false;
    pendingLines = 0;
    pendingHasMbb = false;
    pendingSinceMs = millis();
}

// The session's identity and header, at its first MBB line. The file name
// waits for the first commit, when the clock has had time to be set.
static void sessionName(const String& firstStamp) {
    if (named) return;
    seq++;
    char stem[24];
    snprintf(stem, sizeof stem, "b%04lu-%03d", (unsigned long)bootCount, seq);
    if (sessionPart == 0) { sessionId = stem; sessionPart = 1; }
    else sessionPart++;
    named = true;
    activeBytes = 0;
    String header = (firstStamp.length() ? firstStamp : clockStamp()) +
                    " dongle: session start, " + boardName + ", id " + sessionId + ", part " + String(sessionPart) +
                    ", boot " + String(bootCount) + " (" + bootReason + "), time " + clockSourceName() + ", fw " FW_VERSION;
    if (gz.add(header.c_str(), header.length())) pendingLines++;   // GZ_HEADER_ROOM was kept for it
    else { droppedLines++; Serial.println("store: no room for the session header"); }
}

// The file name, at creation: boot count and sequence from the identity,
// the time from the clock as it is now.
static String fileNameNow() {
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
    int s = seq;
    // A boot count that failed to save repeats, and a repeated name would
    // overwrite a file the puller may already hold; skip past any name in use.
    do {
        ::sessionName(b, sizeof b, (unsigned long)bootCount, s, when.c_str());   // an lfs_stat, a read
    } while (LittleFS.exists(pathOf(b)) && ++s < 1000);
    if (s != seq) seq = s;   // the header keeps the id it was given; only this file's name moves on
    return String(b);
}

enum WriteResult { WROTE, NOT_OPENED, SHORT };

// Everything in the stream's buffer to the file. NOT_OPENED leaves the
// buffer as it was; SHORT means bytes were lost and the stream is broken
// from there.
// retryNow: the forced full-buffer commit and a session end must try
// whatever happened a moment ago; the per-tick retry must not.
static WriteResult writeOut(bool retryNow = false) {
    if (!named) return NOT_OPENED;
    if (gz.overflowed()) return SHORT;   // bytes already lost inside the stream
    if (!active) {
        uint32_t now = millis();
        if (!retryNow && now - lastOpenFailMs < RECLAIM_GAP_MS) return NOT_OPENED;   // a failed creation is not retried every tick
        ensureSpace();   // a no-op above the reserve
        String name = fileNameNow();
        active = LittleFS.open(pathOf(name), FILE_WRITE);
        if (!active) { lastOpenFailMs = now; Serial.printf("store: cannot open %s\n", name.c_str()); return NOT_OPENED; }
        activeName = name;
        Serial.printf("store: session %s\n", activeName.c_str());
    }
    size_t n = gz.pending();
    size_t done = active.write(gz.out, n);
    if (done < n) {
        uint32_t now = millis();
        if (now - lastReclaimMs > RECLAIM_GAP_MS) {   // once: reclaim and try the remainder
            lastReclaimMs = now;
            if (ensureSpace()) done += active.write(gz.out + done, n - done);
        }
    }
    active.flush();
    activeBytes += done;
    size_t onDisk = active.size();
    if (onDisk < activeBytes) {   // the stdio buffer took what the filesystem could not
        Serial.printf("store: commit lost %u byte(s) at the flush, flash full\n", (unsigned)(activeBytes - onDisk));
        activeBytes = onDisk;
        return SHORT;
    }
    gz.taken();
    return done == n ? WROTE : SHORT;
}

// Drop the stream: the file, if any, keeps what reached it.
static void streamDiscard(const char* why) {
    if (active) active.close();
    Serial.printf("store: closed %s (%s)\n", activeName.c_str(), why);
    activeName = "";
    streamOpen = false;
    named = false;
    endWanted = false;
    pendingLines = 0;
    pendingHasMbb = false;
}

// End the session properly: commit what waits, add the closing line, the
// trailer, and write. Never re-enters through commitPending.
static void sessionEnd(const char* why) {
    if (!streamOpen || !named) return;   // an unnamed stream holds only notes, which keep waiting
    // What waits goes first, which also creates the file. If the file cannot
    // be created nothing is lost yet: the session stays open and the next
    // commit tries again, so a full flash costs a session boundary, not lines.
    gz.flush();
    switch (writeOut(true)) {
        case WROTE: pendingLines = 0; break;
        case NOT_OPENED: endWanted = true; return;   // closed at the first commit that gets a file
        case SHORT: droppedLines += pendingLines; streamDiscard("part ends on a write failure"); return;
    }
    String line = clockStamp() + " dongle: " + why;
    gz.add(line.c_str(), line.length());   // the buffer was just emptied
    gz.finish();
    if (writeOut(true) != WROTE) droppedLines += 1;
    streamDiscard(why);
}

// Before a deliberate restart: notes still waiting for a session are lost, counted.
void storeShutdown() {
    Lock l;
    if (streamOpen && named) sessionEnd("session end");
    else if (streamOpen) { droppedLines += pendingLines; streamDiscard("restart"); }
    if (streamOpen) Serial.printf("store: restarting with %lu line(s) unwritten\n", (unsigned long)pendingLines);
    sessionId = "";
    sessionPart = 0;
}

void storeSessionClose() {
    Lock l;
    // The session is over whichever way this goes: the next MBB lines are a new one.
    sessionId = "";
    sessionPart = 0;
    // Notes alone never make a file: with no MBB lines and no file yet, the
    // stream stays open and the notes wait for the next session.
    if (streamOpen && !active && !pendingHasMbb) return;
    sessionEnd("session end");
}

// The stream's buffer to the flash. Notes alone wait for MBB lines to join
// them. `force` is the full-buffer case, where waiting is not an option.
static void commitPending(bool force) {
    if (!streamOpen || gz.pending() == 0) return;
    if (!pendingHasMbb && !active) {
        if (!force) return;
        if (!named) sessionName(String());   // a buffer full of notes gets a file after all; the header's room was kept
    }
    if (!ok) { droppedLines += pendingLines; streamDiscard("no filesystem"); return; }
    gz.flush();
    switch (writeOut(force)) {
        case WROTE: break;
        case NOT_OPENED:
            // Nothing was written and the buffer is intact: keep waiting for
            // space, unless the buffer is full, in which case the lines go
            // and the part they would have been never existed.
            if (!force) return;
            droppedLines += pendingLines;
            if (sessionPart <= 1) { sessionId = ""; sessionPart = 0; } else sessionPart--;
            streamDiscard("no file could be created");
            return;
        case SHORT:
            // The compressed stream cannot be resumed past a hole. This part
            // ends; the session continues in the next one.
            droppedLines += pendingLines;
            streamDiscard("part ends on a write failure");
            return;
    }
    pendingLines = 0;
    pendingHasMbb = false;
    if (endWanted) { endWanted = false; sessionEnd("session end"); return; }
    if (activeBytes >= SESSION_MAX_BYTES) sessionEnd("session continues in the next part");
}

void storeAppend(const String& line, bool fromMbb) {
    Lock l;
    lastLines[lastHead] = line.length() > LAST_LINE_CHARS ? line.substring(0, LAST_LINE_CHARS) : line;
    lastHead = (lastHead + 1) % LAST_LINES;
    if (lastCount < LAST_LINES) lastCount++;
    if (line.length() + 1 > GZ_LINE_CAP) { droppedLines++; return; }   // longer than a line can be
    streamStart();
    // While the session has no header, the buffer keeps room for one, so the
    // header can always be added the moment the first MBB line names the session.
    if (!named && !gz.fits(line.length() + 1 + GZ_HEADER_ROOM)) {
        commitPending(true);   // names the session first (the room is there), then writes the notes
        streamStart();
    }
    if (fromMbb && !named) {
        int sp = line.indexOf(' ');
        sessionName(sp > 0 ? line.substring(0, sp) : String());   // the header takes the first MBB line's own stamp
    }
    if (!gz.add(line.c_str(), line.length())) {
        commitPending(true);   // on the loop task, so allowed; the buffer is full whatever the MBB is doing
        streamStart();
        if (fromMbb && !named) sessionName(String());
        if (!gz.add(line.c_str(), line.length())) { droppedLines++; return; }
    }
    if (fromMbb && !pendingHasMbb) pendingSinceMs = millis();   // the 15 s bound counts from the first MBB line
    pendingLines++;
    if (fromMbb) pendingHasMbb = true;
}

void storeTick(bool mbbQuiet) {
    Lock l;
    uint32_t now = millis();
    if (pendingLines && pendingHasMbb && (mbbQuiet || now - pendingSinceMs > MAX_PENDING_MS)) commitPending(false);
    if (!ok) return;   // nothing to reclaim on a filesystem that is not there
    if (now - lastRotateMs > 60000) {
        bool low = false;
        if (!mbbQuiet && now - lastSpaceCheckMs > 5000) { lastSpaceCheckMs = now; low = freeBytes() < FS_MIN_FREE / 2; }   // a full traversal: not every tick
        if (mbbQuiet || low) { lastRotateMs = now; ensureSpace(); }
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

static bool nameOk(const String& name) { return logNameOk(name.c_str(), name.length()); }

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

void storeNoteEdge(bool awake) {
    Lock l;
    if (awake) { lastAwake = clockStamp(); awakeCount++; }
    else lastAsleep = clockStamp();
}

String storeEdges() {
    Lock l;
    return "\"last_awake\":\"" + lastAwake + "\",\"last_asleep\":\"" + lastAsleep + "\",\"awake_count\":" + String(awakeCount);
}

uint32_t storeBootCount() { return bootCount; }
uint32_t storeDroppedLines() { return droppedLines; }
bool storeOk() { return ok; }
uint32_t storeFormats() { return formats; }
