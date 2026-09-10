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
#include "sys.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pure/names.h"
#include "pure/keep.h"
#include "pure/zstream.h"
#include "pure/dictkeeper.h"

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
static ZStream<GZ_DICT, GZ_HISTORY, GZ_LINE_CAP, GZ_OUT, GZ_HASH_BITS> gz;   // fixed arrays, never on the heap
static DictKeeper<GZ_DICT, DICT_CAND, DICT_MAX_LINES, GZ_LINE_CAP> keeper;    // the dictionary, learned from the sessions
static uint32_t dictId = 0;         // the dictionary on flash and in use, by its Adler-32; 0 for none
static uint32_t lastDictRefreshMs = 0;
static bool streamOpen = false;  // gz is begun
static bool named = false;       // the session has its name and header
static bool endWanted = false;   // the session ended before its file could be created; close at the first commit
static bool pendingHasMbb = false;
static uint32_t pendingLines = 0;   // lines in gz's buffer since the last commit
static uint32_t pendingSinceMs = 0;
static uint32_t droppedLines = 0;
static uint32_t rawBytes = 0;      // console bytes taken in since boot
static uint32_t storedBytes = 0;   // compressed bytes that reached the flash since boot
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

template <class S>
static bool isOpenForRead(const S& name) {
    for (auto& n : openForRead) if (n == name) return true;
    return false;
}

// One pass over the directory through POSIX readdir: nothing of it held in
// RAM, no File object, and a stat (a directory lookup per entry) only when
// the caller wants sizes.
template <class F>
static void forEachFile(F&& fn, bool withSizes) {
    const char* mp = LittleFS.mountpoint();
    if (!mp) return;
    char base[64];
    snprintf(base, sizeof base, "%s%s", mp, LOG_DIR);
    DIR* d = opendir(base);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_type == DT_DIR) continue;
        size_t size = 0;
        if (withSizes) {
            char path[sizeof base + 258];
            snprintf(path, sizeof path, "%s/%s", base, e->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            size = (size_t)st.st_size;
        }
        fn(e->d_name, size);
    }
    closedir(d);
}

// The partition's size never changes; asking is a full traversal.
static size_t totalBytes() {
    static size_t total = 0;
    if (!total) total = LittleFS.totalBytes();
    return total;
}

// The board's own files, not the bike's: never listed, reclaimed or deleted.
// The dictionaries and the saved poll batch. The poll batch sits in the root
// rather than the log directory, where nothing walks anyway; the name is
// matched here so that stays true if it ever moves.
static bool houseFile(const char* n) { return strncmp(n, "dict-", 5) == 0 || strcmp(n, POLL_SAVE_NAME) == 0; }

void storeForEachFile(void (*fn)(void*, const char*, size_t, bool), void* ctx) {
    Lock l;
    forEachFile([&](const char* n, size_t size) { if (!houseFile(n)) fn(ctx, n, size, activeName == n); }, true);
}

static size_t freeBytes() { return totalBytes() - LittleFS.usedBytes(); }   // usedBytes is a full-filesystem block traversal: ask it rarely

static size_t fileSizeOf(const char* name) {   // one directory lookup, far cheaper than a freeBytes traversal
    const char* mp = LittleFS.mountpoint();
    if (!mp) return 0;
    char path[160];
    snprintf(path, sizeof path, "%s%s/%s", mp, LOG_DIR, name);
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

// The flash the delete actually gives back: a file's data rounded up to whole
// erase blocks, and at least one block, whatever a byte count says. A lower
// bound (metadata frees on top), so a tally of these never overstates the space
// returned, but tight enough that the reclaim stops at the minimum files rather
// than over-deleting the oldest to make a raw-byte sum catch up. The block is
// the ESP32 flash sector; a larger real block only makes the bound safer.
static const size_t FS_BLOCK = 4096;
static size_t blockBytes(size_t sz) { return sz ? ((sz + FS_BLOCK - 1) & ~(FS_BLOCK - 1)) : FS_BLOCK; }

static size_t dictCollect();   // returns the bytes the collected dictionaries freed

// Delete oldest first until the reserve is back, without a full-filesystem
// traversal after every delete. Stale dictionaries go first (dictCollect, whose
// walk is name-only). Then a name-only walk picks the eight oldest deletable
// sessions and they go in order; deleting a file frees at least its own size,
// since the flash rounds up to whole blocks, so summing the raw sizes is a lower
// bound on the space returned, and only once that bound covers the shortfall is
// a single freeBytes() spent to confirm it, rather than one per delete (each is
// a full-filesystem block traversal, which was the bulk of a deep reclaim's
// time). A name that will not delete is skipped from then on, three refusals in
// a row end the attempt, and a second round is the rare case where eight was
// not enough.
static bool ensureSpace() {
    size_t free = freeBytes();
    if (free >= FS_MIN_FREE) return false;
    size_t need = FS_MIN_FREE - free;   // bytes still to free
    size_t freed = dictCollect();       // stale dictionaries cost nothing to keep off, and go first; their bytes join the tally
    char floor[65] = "";   // names at or below this were tried and refused
    int refusals = 0;
    for (int round = 0; round < 4; round++) {
        if (freed >= need) {   // the deletes so far are a lower bound that covers the shortfall: confirm once, since block rounding could leave a little
            free = freeBytes();
            if (free >= FS_MIN_FREE) return true;
            need = FS_MIN_FREE - free;
            freed = 0;
        }
        sysFeedWatchdog();
        KeepSmallest<8, 65> oldest;   // names sort by creation, so the smallest are the oldest
        forEachFile([&](const char* name, size_t) {
            if (houseFile(name) || activeName == name || isOpenForRead(name)) return;
            if (floor[0] && strcmp(name, floor) <= 0) return;
            oldest.offer(name);
        }, false);
        size_t n = oldest.n;
        if (n == 0) break;
        for (size_t i = 0; i < n && freed < need; i++) {
            sysFeedWatchdog();
            strlcpy(floor, oldest.item[i], sizeof floor);
            size_t sz = fileSizeOf(oldest.item[i]);
            if (!LittleFS.remove(pathOf(oldest.item[i]))) {
                Serial.printf("store: cannot delete %s\n", oldest.item[i]);
                if (++refusals >= 3) { Serial.println("store: giving up on the reclaim for now"); return false; }
                continue;
            }
            refusals = 0;
            freed += blockBytes(sz);
            Serial.printf("store: deleted %s for space\n", oldest.item[i]);
        }
    }
    if (freeBytes() >= FS_MIN_FREE) return true;
    Serial.println("store: the reserve is not back; every file is active, being read, or will not delete");
    return false;
}

bool storeBegin(const char* resetReason) {
    mtx = xSemaphoreCreateRecursiveMutex();
    if (!mtx) { Serial.println("store: no memory for the lock"); return false; }
    bootReason = resetReason;
    strlcpy(boardName, sysNodeName(), sizeof boardName);
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
    p.begin("dongle", true);
    uint32_t id = p.getUInt("dict", 0);
    p.end();
    if (id) {
        char name[24];
        snprintf(name, sizeof name, "dict-%08lx.txt", (unsigned long)id);
        File f = LittleFS.open(pathOf(name), FILE_READ);
        size_t n = f ? f.read(gz.scratch(), GZ_DICT) : 0;
        if (f) f.close();
        if (n && uzlib_adler32(gz.scratch(), n, 1) == id) { keeper.load(gz.scratch(), n); dictId = id; }
        else Serial.printf("store: dictionary %s missing or damaged; starting without one\n", name);
    }
    Serial.printf("store: dictionary %08lx, %u bytes, %u lines\n", (unsigned long)dictId, (unsigned)keeper.dlen, (unsigned)keeper.nlines);
    return true;
}

// The dictionary files a session file still needs are found by the id in its
// name; the rest go, except the one in use. One name-only walk gathers both the
// ids the sessions still name and the dictionary files themselves, then the
// unnamed ones are deleted. With more distinct ids in use than the array holds,
// nothing goes: a needed dictionary could be in the overflow.
static size_t dictCollect() {
    uint32_t inUse[32];
    int nInUse = 0;
    bool overflow = false;
    struct { char name[24]; uint32_t id; } dicts[32];   // as many as inUse can hold; beyond that a needed one might be missed, and inUse overflows first anyway
    int nDicts = 0;
    forEachFile([&](const char* n, size_t) {
        if (strncmp(n, "dict-", 5) == 0) {
            if (nDicts < 32) { strlcpy(dicts[nDicts].name, n, sizeof dicts[nDicts].name); dicts[nDicts].id = strtoul(n + 5, nullptr, 16); nDicts++; }
            return;
        }
        uint32_t id = logDictId(n, strlen(n));   // the dictionary named in the file's own name, no open needed
        if (id == 0) return;   // not a session file, or one that names no dictionary
        for (int i = 0; i < nInUse; i++) if (inUse[i] == id) return;
        if (nInUse < 32) inUse[nInUse++] = id; else overflow = true;
    }, false);
    if (overflow) { Serial.println("store: dictionaries not collected: more than 32 in use"); return 0; }
    size_t freed = 0;
    for (int i = 0; i < nDicts; i++) {
        if (dictId && dicts[i].id == dictId) continue;
        if (isOpenForRead(dicts[i].name)) continue;
        bool used = false;
        for (int j = 0; j < nInUse; j++) if (inUse[j] == dicts[i].id) { used = true; break; }
        if (used) continue;
        size_t sz = fileSizeOf(dicts[i].name);
        if (LittleFS.remove(pathOf(dicts[i].name))) { freed += blockBytes(sz); Serial.printf("store: dictionary %s no longer needed\n", dicts[i].name); }
    }
    return freed;
}

// At a session's end with the MBB asleep: if the session taught enough,
// rebuild the dictionary, write it as a file the puller can fetch by id,
// and use it from the next session on.
static void dictMaybeRefresh() {
    if (!ok || streamOpen) return;   // no filesystem, or the stream still holds a session and the scratch space is in use
    bool due = keeper.noveltyBytes() >= DICT_NOVELTY && (lastDictRefreshMs == 0 || millis() - lastDictRefreshMs > DICT_REFRESH_MIN_MS);
    if (!due) { keeper.endSession(DICT_MARK_SESSIONS); return; }
    size_t newBytes = 0;
    size_t n = keeper.rebuild(gz.scratch(), GZ_DICT, &newBytes);
    uint32_t id = uzlib_adler32(gz.scratch(), n, 1);
    if (n == 0 || id == dictId || newBytes == 0) { keeper.endSession(DICT_MARK_SESSIONS); return; }   // nothing learned: no file
    char name[24];
    snprintf(name, sizeof name, "dict-%08lx.txt", (unsigned long)id);
    const char* tmpName = "dict-new.tmp";
    ensureSpace();   // a no-op above the reserve
    // Written to a temporary name, flushed, and read back against its id
    // before anything names it: a dictionary the puller cannot verify would
    // cost every session that named it, and a name already on the flash is
    // never truncated.
    bool good;
    if (LittleFS.exists(pathOf(name))) {
        // The same id is already on the flash: adopt what the flash holds,
        // read into the scratch, rather than trust a checksum match alone.
        File r = LittleFS.open(pathOf(name), FILE_READ);
        size_t got = r ? r.read(gz.scratch(), GZ_DICT) : 0;
        if (r) r.close();
        good = got == n && uzlib_adler32(gz.scratch(), got, 1) == id;
    } else {
        File f = LittleFS.open(pathOf(tmpName), FILE_WRITE);
        good = f && f.write(gz.scratch(), n) == n;
        if (f) { f.flush(); good = good && f.size() == n; f.close(); }
        if (good) good = LittleFS.rename(pathOf(tmpName), pathOf(name));
        if (!good) LittleFS.remove(pathOf(tmpName));
    }
    if (good) {
        File r = LittleFS.open(pathOf(name), FILE_READ);
        uint8_t buf[256];
        uint32_t check = 1;
        size_t got = 0;
        while (r) { size_t k = r.read(buf, sizeof buf); if (!k) break; check = uzlib_adler32(buf, k, check); got += k; }
        if (r) r.close();
        good = got == n && check == id;
    }
    Preferences p;
    bool named = good && p.begin("dongle", false) && p.putUInt("dict", id) == sizeof(uint32_t);
    p.end();
    if (!named) {
        Serial.println("store: dictionary not written; keeping the old one");
        keeper.endSession(DICT_MARK_SESSIONS);
        return;
    }
    keeper.load(gz.scratch(), n);
    dictId = id;
    lastDictRefreshMs = millis() ? millis() : 1;
    Serial.printf("store: dictionary %s, %u bytes, %u lines, %u new\n", name, (unsigned)n, (unsigned)keeper.nlines, (unsigned)newBytes);
    dictCollect();
}

// A session is a stream in RAM until it earns a file. The stream starts at
// the first line of any kind so that dongle notes can wait in it; the name
// and the header are settled at the first MBB line, when the clock and the
// session are real; the file itself is created at the first commit, so the
// flash is not touched while the MBB is talking.
static void streamStart() {
    if (streamOpen) return;
    gz.begin(keeper.dict, keeper.dlen);
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
    char d[16];
    snprintf(d, sizeof d, ", dict %08lx", (unsigned long)dictId);
    header += d;
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
        ::sessionName(b, sizeof b, (unsigned long)bootCount, s, when.c_str(), (unsigned long)dictId);   // an lfs_stat, a read
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
    storedBytes += done;
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
    dictMaybeRefresh();   // the MBB is asleep: the one time a dictionary may be written
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
    rawBytes += line.length() + 1;
    if (fromMbb) keeper.note(line.c_str(), line.length());
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

static bool nameOk(const String& name) { return logNameOk(name.c_str(), name.length()); }

StoreDeleteResult storeDelete(const String& name) {
    Lock l;
    if (!nameOk(name) || name == activeName || isOpenForRead(name)) return STORE_REFUSED;
    if (houseFile(name.c_str())) return STORE_REFUSED;   // the board's own: dictionaries are collected on their own, the poll batch is the poller's
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
    total = ok ? totalBytes() : 0;
    used = ok ? LittleFS.usedBytes() : 0;
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

String storeMetricsJson() {
    Lock l;
    static uint32_t cachedAtMs = 0;
    static uint32_t files = 0, bytes = 0;
    if (ok && (cachedAtMs == 0 || millis() - cachedAtMs > 10000)) {   // a directory walk: not on every status call
        cachedAtMs = millis() ? millis() : 1;
        files = 0;
        forEachFile([&](const char*, size_t) { files++; }, false);
    }
    size_t total = ok ? totalBytes() : 0, used = ok ? LittleFS.usedBytes() : 0;
    bytes = used;   // the files, plus a little metadata; a stat per file would cost a directory lookup each
    float ratio = storedBytes ? (float)rawBytes / storedBytes : 0;
    uint32_t upS = millis() / 1000;
    long daysLeft = -1;   // meaningful once an hour of rate is known
    if (upS > 3600 && storedBytes > 0 && total > used) daysLeft = (long)((double)(total - used) / storedBytes * upS / 86400.0);
    char b[260];
    snprintf(b, sizeof b, "{\"files\":%lu,\"bytes\":%lu,\"fs_used\":%u,\"fs_total\":%u,\"raw_bytes\":%lu,\"stored_bytes\":%lu,\"ratio\":%.1f,\"days_left\":%ld,\"dict\":\"%08lx\",\"dict_bytes\":%u,\"dict_lines\":%u,\"novelty\":%u}",
             (unsigned long)files, (unsigned long)bytes, (unsigned)used, (unsigned)total, (unsigned long)rawBytes, (unsigned long)storedBytes, ratio, daysLeft,
             (unsigned long)dictId, (unsigned)keeper.dlen, (unsigned)keeper.nlines, (unsigned)keeper.noveltyBytes());
    return String(b);
}

uint32_t storeBootCount() { return bootCount; }
uint32_t storeDroppedLines() { return droppedLines; }
bool storeOk() { return ok; }
uint32_t storeFormats() { return formats; }
