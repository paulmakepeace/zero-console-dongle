// The MBB's own time stamp, and the two-stamp agreement rule that lets it
// move the clock. Plain C++, no Arduino, so it runs under the host tests.
#pragma once
#include <cstddef>
#include <cstring>

struct MbbStamp {
    int year, month, day, hour, minute, second, ms;   // month 1..12
};

inline int daysInMonth(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : d[month - 1];
}

// A stamp at the start of the line, after an optional "DEBUG:" and spaces:
// MM/DD/YYYY hh:mm:ss.mmm. Every field is range-checked, including the day
// against its month, so a corrupted digit cannot become a real date.
inline bool parseMbbStamp(const char* s, size_t len, MbbStamp& out) {
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
    auto num = [](const char* p, int n) { int v = 0; while (n--) v = v * 10 + (*p++ - '0'); return v; };
    const char* p = s + i;
    MbbStamp t = {num(p + 6, 4), num(p, 2), num(p + 3, 2), num(p + 11, 2), num(p + 14, 2), num(p + 17, 2), num(p + 20, 3)};
    if (t.year < 2024 || t.year > 2040 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > daysInMonth(t.year, t.month) ||
        t.hour > 23 || t.minute > 59 || t.second > 59) return false;
    out = t;
    return true;
}

// Two consecutive stamps have to tell the same story, allowing for the time
// that passed between them, before the clock moves. offer() returns true on
// the stamp that confirms the previous one.
struct StampConsensus {
    bool have = false;
    long candidateSec = 0;
    unsigned long candidateMs = 0;

    bool offer(long sec, unsigned long nowMs, long toleranceS = 5) {
        if (have) {
            long expected = candidateSec + (long)((nowMs - candidateMs) / 1000);
            long delta = sec - expected;
            if (delta > -toleranceS && delta < toleranceS) { have = false; return true; }
        }
        have = true;
        candidateSec = sec;
        candidateMs = nowMs;
        return false;
    }
    void reset() { have = false; }
};
