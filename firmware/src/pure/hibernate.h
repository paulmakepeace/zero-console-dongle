// The MBB's own announcement of its sleep, and the arithmetic for waking the
// dongle just before the MBB does.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// "Saving Stats, Hibernating for 3600 sec" -> 3600; -1 when the line is not that.
inline long parseHibernateSeconds(const char* s, size_t len) {
    static const char key[] = "Hibernating for ";
    const size_t klen = sizeof(key) - 1;
    for (size_t i = 0; i + klen <= len; i++) {
        if (memcmp(s + i, key, klen) != 0) continue;
        size_t j = i + klen;
        long v = 0;
        size_t digits = 0;
        while (j < len && s[j] >= '0' && s[j] <= '9' && digits < 8) { v = v * 10 + (s[j] - '0'); j++; digits++; }
        if (digits == 0) return -1;
        while (j < len && s[j] == ' ') j++;
        return (j + 3 <= len && memcmp(s + j, "sec", 3) == 0) ? v : -1;
    }
    return -1;
}

inline bool lineHas(const char* s, size_t len, const char* k) {
    size_t n = strlen(k);
    for (size_t i = 0; i + n <= len; i++) if (memcmp(s + i, k, n) == 0) return true;
    return false;
}

inline bool isKeyOn(const char* s, size_t len) { return lineHas(s, len, "Key Sw = ON"); }

// The lines that say the bike is looked after: the cellular module answered
// and the 12 V battery was topped up, or someone turned the key.
inline bool isBikeAttended(const char* s, size_t len) {
    return isKeyOn(s, len) || lineHas(s, len, "12V successfully charged") || lineHas(s, len, "CCM RTC verified OK");
}

// Seconds until the MBB's timer fires: its announcement plus its own count,
// less what has passed. With no announcement known, fallbackS. Negative
// means the wake is overdue.
inline long secondsUntilMbbWake(bool haveHibernate, long hibernateAtS, long hibernateS, long nowS, long fallbackS) {
    if (!haveHibernate) return fallbackS;
    return hibernateAtS + hibernateS - nowS;
}

// How long to sleep: most of the way, with a margin that covers the sleep
// timer's RC clock running long, and 0 when the wake is too close to be
// worth the network round trip. Waking a few minutes early costs a few
// milliamp-hours a day; timing it closely would cost the code its simplicity.
inline long sleepSeconds(long untilWakeS, long marginPct, long minS) {
    long s = (long)((int64_t)untilWakeS * (100 - marginPct) / 100);
    return s >= minS ? s : 0;
}

// One timed sleep per announcement. The sleep timer's clock runs a few per
// cent long and the dongle's own clock is advanced by the planned time, not
// the real one, so after a timer wake the remainder it reads is too long: a
// second sleep planned from it lands on or after the MBB. The dongle sleeps
// once for nine tenths of the wait and stays up for the rest. A wake on pin 8
// leaves the plan open, so a glitch with no MBB session behind it does not
// cost the rest of the hour; a real session ends with a new announcement.
// An announcement the MBB never honours goes stale a minute after its time
// and the fallback takes over.
struct SleepPlan {
    bool haveHib = false;
    long hibAtS = 0;
    long hibSec = 0;
    bool sleptForHib = false;

    void noteHibernate(long nowS, long seconds) { haveHib = true; hibAtS = nowS; hibSec = seconds; sleptForHib = false; }
    long until(long nowS, long fallbackS) const { return secondsUntilMbbWake(haveHib, hibAtS, hibSec, nowS, fallbackS); }
    // Seconds to sleep now, 0 to stay up. The MBB's own announced count is
    // the plan; the fallback is only for having none. A wait beyond a day is
    // not something the MBB can have meant, from a mangled digit in the count
    // or a clock stepped backwards under a plan held in wall time, and that
    // sanity bound is deliberately not the fallback: clamping to the guess
    // would throw away a longer interval the bike genuinely announced.
    static const long SANE_MAX_S = 86400;
    long next(long nowS, long fallbackS, long marginPct, long minS) {
        long u = until(nowS, fallbackS);
        if (haveHib && (u < -60 || u > SANE_MAX_S)) { haveHib = false; sleptForHib = false; u = fallbackS; }
        if (haveHib && sleptForHib) return 0;
        return sleepSeconds(u, marginPct, minS);
    }
    void slept(bool onTimer) { if (onTimer && haveHib) sleptForHib = true; }
};

// The long-term storage mode from either line the MBB prints about it:
// "LTSM state: INIT to DIS" at every wake, whose last word is the state,
// and "storage mode      Inactive" in the bms snapshot. 1 for on, -1 for
// off, 0 for a line that says nothing. DIS and Inactive are the spellings
// for off, EN and Active for on, EN_PEND and DIS_PEND on the way in and
// out; the wake-time line with the mode on is not captured, so any state
// but DIS and INIT counts as on.
inline int storageModeFromLine(const char* s, size_t len) {
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\r' || s[len - 1] == '\t')) len--;
    auto after = [&](const char* key) -> long {
        size_t n = strlen(key);
        for (size_t i = 0; i + n <= len; i++) if (memcmp(s + i, key, n) == 0) return (long)(i + n);
        return -1;
    };
    auto is = [&](size_t from, size_t to, const char* w) { size_t n = strlen(w); return to - from == n && memcmp(s + from, w, n) == 0; };
    long p = after("LTSM state:");
    if (p >= 0) {
        size_t t = len;
        while (t > (size_t)p && s[t - 1] != ' ') t--;
        if (t >= len) return 0;
        if (is(t, len, "DIS")) return -1;
        if (is(t, len, "INIT")) return 0;
        // Any state but those two means on, so the word has to look like one
        // of the MBB's own: a help line that happens to quote the phrase is
        // not the bike telling us it is in storage.
        for (size_t i = t; i < len; i++) {
            char c = s[i];
            if (!((c >= 'A' && c <= 'Z') || c == '_')) return 0;
        }
        return 1;
    }
    p = after("storage mode");
    if (p >= 0) {
        size_t t = (size_t)p;
        while (t < len && s[t] == ' ') t++;
        size_t e = t;
        while (e < len && s[e] != ' ') e++;
        if (is(t, e, "Inactive")) return -1;
        if (is(t, e, "Active")) return 1;
    }
    return 0;
}
