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

// Seconds until the MBB's timer fires: its announcement plus its own count,
// less what has passed. Time spent asleep was measured by the RC clock, so
// unless NTP has since put the clock right it is assumed to have run long
// by driftPct: the answer errs early. With no announcement known,
// fallbackS. Negative means the wake is overdue.
inline long secondsUntilMbbWake(bool haveHibernate, long hibernateAtS, long hibernateS, long nowS,
                                long uncorrectedSleptS, long driftPct, long fallbackS) {
    if (!haveHibernate) return fallbackS;
    long elapsed = nowS - hibernateAtS + uncorrectedSleptS * driftPct / 100;
    return hibernateS - elapsed;
}

// How long to sleep now. The sleep timer runs on an RC clock that is a few
// percent off, so a long wait is taken in chunks, each wake letting NTP put
// the clock right, and every chunk is shortened by the lead plus the drift
// it could carry, so the last one still lands before the MBB. 0 means do
// not sleep: the wake is inside the lead, or the chunk would be too short
// to be worth the network round trip.
inline long sleepChunk(long untilWakeS, long leadS, long maxChunkS, long driftPct, long minS) {
    if (untilWakeS <= leadS) return 0;
    long chunk = untilWakeS < maxChunkS ? untilWakeS : maxChunkS;
    long sleep = chunk - leadS - chunk * driftPct / 100;
    return sleep >= minS ? sleep : 0;
}
