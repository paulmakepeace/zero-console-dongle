// Wall time without a battery. NTP owns the clock while its fix is fresh;
// otherwise the MBB's own stamps do, which it prints at the start of most
// lines as MM/DD/YYYY hh:mm:ss.mmm in local time. A stamp is only believed
// at the start of a line, and two consecutive stamps have to agree before
// the clock is stepped, so a log dump full of old dates or one corrupted
// digit cannot move it. Every step is written into the log.
#include "clock.h"
#include "config.h"
#include <sys/time.h>
#include <time.h>
#include "esp_sntp.h"

static volatile TimeSource source = TIME_NONE;
static volatile uint32_t lastNtpSyncMs = 0;
static volatile bool ntpSyncPending = false;
static ClockNoteHandler noteHandler;
static bool haveCandidate = false;
static time_t candidateSec;
static uint32_t candidateMs;
static String tzSetting, ntpSetting;

static void onSntpSync(struct timeval*) {
    source = TIME_NTP;
    lastNtpSyncMs = millis();
    ntpSyncPending = true;
}

void clockBegin(const char* tz, const char* ntpServer, ClockNoteHandler onNote) {
    noteHandler = onNote;
    tzSetting = tz;
    ntpSetting = ntpServer;
    esp_sntp_set_time_sync_notification_cb(onSntpSync);
    configTzTime(tzSetting.c_str(), ntpSetting.c_str());
}

void clockNetworkUp() {
    configTzTime(tzSetting.c_str(), ntpSetting.c_str());   // restarts SNTP; the first request goes out now
}

bool clockValid() { return source != TIME_NONE; }
TimeSource clockSource() { return source; }

const char* clockSourceName() {
    switch (source) {
        case TIME_NTP: return "ntp";
        case TIME_MBB: return "mbb";
        default: return "none";
    }
}

uint32_t clockNtpAgeS() {
    if (source != TIME_NTP) return UINT32_MAX;
    return (millis() - lastNtpSyncMs) / 1000;
}

static bool ntpFresh() {
    return source == TIME_NTP && (millis() - lastNtpSyncMs) < NTP_FRESH_S * 1000UL;
}

String clockStamp() {
    char buf[40];
    if (source == TIME_NONE) {
        uint32_t ms = millis();
        snprintf(buf, sizeof buf, "u%06lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
        return String(buf);
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    if (!localtime_r(&tv.tv_sec, &tm)) return String("u?");
    size_t n = strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    if (n == 0) return String("u?");
    snprintf(buf + n, sizeof buf - n, ".%03ld", (long)(tv.tv_usec / 1000));
    return String(buf);
}

static int num(const char* p, int n) {
    int v = 0;
    while (n--) v = v * 10 + (*p++ - '0');
    return v;
}

// A stamp at the start of the line, after an optional "DEBUG:" and spaces.
static bool parseMbbTime(const char* s, size_t len, struct timeval* out) {
    static const char pat[] = "dd/dd/dddd dd:dd:dd.ddd";
    const size_t plen = sizeof(pat) - 1;
    size_t i = 0;
    if (len >= 6 && memcmp(s, "DEBUG:", 6) == 0) i = 6;
    while (i < len && s[i] == ' ') i++;
    if (i + plen > len) return false;
    for (size_t j = 0; j < plen; j++) {
        char c = s[i + j];
        if (pat[j] == 'd' ? (c < '0' || c > '9') : (c != pat[j])) return false;
    }
    const char* p = s + i;
    struct tm tm = {};
    tm.tm_mon = num(p, 2) - 1;
    tm.tm_mday = num(p + 3, 2);
    tm.tm_year = num(p + 6, 4) - 1900;
    tm.tm_hour = num(p + 11, 2);
    tm.tm_min = num(p + 14, 2);
    tm.tm_sec = num(p + 17, 2);
    tm.tm_isdst = -1;
    if (tm.tm_year < 2024 - 1900 || tm.tm_year > 2040 - 1900 || tm.tm_mon < 0 || tm.tm_mon > 11 ||
        tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59) return false;
    struct tm check = tm;
    time_t t = mktime(&tm);
    if (t < 1700000000 || tm.tm_mday != check.tm_mday) return false;   // mktime rolled an impossible date
    out->tv_sec = t;
    out->tv_usec = num(p + 20, 3) * 1000;
    return true;
}

void clockMaybeSetFromMbb(const char* line, size_t len) {
    if (ntpFresh()) return;
    struct timeval tv;
    if (!parseMbbTime(line, len, &tv)) return;
    struct timeval now;
    gettimeofday(&now, nullptr);
    if (source != TIME_NONE) {
        long diff = (long)(now.tv_sec - tv.tv_sec);
        if (diff > -5 && diff < 5) { haveCandidate = false; return; }   // agrees, nothing to do
    }
    // Two consecutive stamps have to tell the same story, allowing for the
    // time that passed between them, before the clock moves.
    uint32_t nowMs = millis();
    if (haveCandidate) {
        long expected = (long)candidateSec + (long)((nowMs - candidateMs) / 1000);
        long delta = (long)tv.tv_sec - expected;
        if (delta > -5 && delta < 5) {
            String before = clockStamp();
            settimeofday(&tv, nullptr);
            if (source != TIME_NTP) source = TIME_MBB;   // an NTP sync may have landed meanwhile
            haveCandidate = false;
            if (noteHandler) {
                String note = "dongle: clock stepped from " + before + " to " + clockStamp() + " by the MBB";
                noteHandler(note.c_str());
            }
            return;
        }
    }
    haveCandidate = true;
    candidateSec = tv.tv_sec;
    candidateMs = nowMs;
}

void clockTick() {
    if (ntpSyncPending && noteHandler) {
        ntpSyncPending = false;
        String note = "dongle: clock set from ntp, now " + clockStamp();
        noteHandler(note.c_str());
    }
}
