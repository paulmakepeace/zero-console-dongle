// The MBB's own announcement of its sleep, and the arithmetic for waking the
// dongle just before the MBB does.
#pragma once
#include <cstddef>
#include <cstring>

// "Saving Stats, Hibernating for 3600 sec" -> 3600; -1 when the line is not that.
inline long parseHibernateSeconds(const char* s, size_t len) {
    static const char key[] = "Hibernating for ";
    const size_t klen = sizeof(key) - 1;
    for (size_t i = 0; i + klen < len; i++) {
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

// The lines that say the bike is looked after: the cellular module answered
// and the 12 V battery was topped up, or someone turned the key.
inline bool isBikeAttended(const char* s, size_t len) {
    static const char* const keys[] = {"12V successfully charged", "CCM RTC verified OK", "Key Sw = ON"};
    for (const char* k : keys) {
        size_t n = strlen(k);
        for (size_t i = 0; i + n <= len; i++) if (memcmp(s + i, k, n) == 0) return true;
    }
    return false;
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
    long s = untilWakeS * (100 - marginPct) / 100;
    return s >= minS ? s : 0;
}
