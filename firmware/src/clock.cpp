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
#include "pure/mbb_time.h"

// source and lastNtpSyncMs belong to the loop task; the SNTP callback, on
// the lwIP task, only raises the flag and the loop promotes it in clockTick.
static TimeSource source = TIME_NONE;
static uint32_t lastNtpSyncMs = 0;
static volatile bool ntpSyncPending = false;
static ClockNoteHandler noteHandler;
static StampConsensus consensus;
// Fixed storage: lwIP keeps the server name pointer, so it must never move.
static char tzSetting[64], ntpSetting[65];

static void onSntpSync(struct timeval*) { ntpSyncPending = true; }

void clockBegin(const char* tz, const char* ntpServer, ClockNoteHandler onNote) {
    noteHandler = onNote;
    strlcpy(tzSetting, tz, sizeof tzSetting);
    strlcpy(ntpSetting, ntpServer, sizeof ntpSetting);
    esp_sntp_set_time_sync_notification_cb(onSntpSync);
    configTzTime(tzSetting, ntpSetting);
}

void clockNetworkUp() {
    configTzTime(tzSetting, ntpSetting);   // restarts SNTP; the first request goes out now
}

void clockApplySettings(const char* tz, const char* ntpServer) {
    esp_sntp_stop();   // release the old server name before it is overwritten
    strlcpy(tzSetting, tz, sizeof tzSetting);
    strlcpy(ntpSetting, ntpServer, sizeof ntpSetting);
    configTzTime(tzSetting, ntpSetting);
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

void clockSlept() {
    if (source == TIME_NTP) lastNtpSyncMs = millis() - NTP_FRESH_S * 1000UL;
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

// The stamp is local time in the configured zone; mktime turns it into epoch.
static bool parseMbbTime(const char* s, size_t len, struct timeval* out) {
    MbbStamp st;
    if (!parseMbbStamp(s, len, st)) return false;
    struct tm tm = {};
    tm.tm_year = st.year - 1900;
    tm.tm_mon = st.month - 1;
    tm.tm_mday = st.day;
    tm.tm_hour = st.hour;
    tm.tm_min = st.minute;
    tm.tm_sec = st.second;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t < 1700000000) return false;
    out->tv_sec = t;
    out->tv_usec = st.ms * 1000;
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
        if (diff > -5 && diff < 5) { consensus.reset(); return; }   // agrees, nothing to do
    }
    if (!consensus.offer((long)tv.tv_sec, millis())) return;   // the first of two, or a disagreement
    if (ntpSyncPending) return;   // NTP just landed; the tick promotes it
    String before = clockStamp();
    settimeofday(&tv, nullptr);
    source = TIME_MBB;
    if (noteHandler) {
        String note = "dongle: clock stepped from " + before + " to " + clockStamp() + " by the MBB";
        noteHandler(note.c_str());
    }
}

void clockTick() {
    // SNTP sets the system clock itself and then raises this flag; the loop
    // only promotes the source. An MBB stamp cannot have overwritten the sync,
    // since clockMaybeSetFromMbb stands down while it is pending.
    if (!ntpSyncPending) return;
    ntpSyncPending = false;
    source = TIME_NTP;
    lastNtpSyncMs = millis();
    if (noteHandler) {
        String note = "dongle: clock set from ntp, now " + clockStamp();
        noteHandler(note.c_str());
    }
}
