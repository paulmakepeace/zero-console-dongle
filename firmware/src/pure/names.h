// Log file names: what the HTTP side accepts, and how a session is named.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>

// One path segment: alphanumeric first, then alphanumerics, '-', '_' and '.',
// at most 64 characters and never "..".
inline bool logNameOk(const char* name, size_t len) {
    if (len == 0 || len > 64 || !isalnum((unsigned char)name[0])) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = name[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    for (size_t i = 1; i < len; i++) if (name[i] == '.' && name[i - 1] == '.') return false;
    return true;
}

// bBBBB-SSS-<when>-<dict>.log.z: boot count and sequence first so names sort by
// creation; the dictionary's Adler-32 last, so the store can collect unused
// dictionaries by reading names rather than opening every file for its header.
inline int sessionName(char* out, size_t cap, unsigned long boot, int seq, const char* when, unsigned long dictId) {
    return snprintf(out, cap, "b%04lu-%03d-%s-%08lx.log.z", boot, seq, when, dictId);
}

// The dictionary id from a log name, the 8 hex digits before ".log.z"; 0 when
// the name carries none (a file written with no dictionary, or one named
// before this suffix existed, both of which name no dictionary to protect).
inline uint32_t logDictId(const char* n, size_t len) {
    static const char suf[] = ".log.z";
    const size_t sl = sizeof(suf) - 1;
    if (len < sl + 9 || memcmp(n + len - sl, suf, sl) != 0) return 0;
    size_t e = len - sl;               // one past the last hex digit
    if (n[e - 9] != '-') return 0;
    uint32_t id = 0;
    for (size_t i = e - 8; i < e; i++) {
        char c = n[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) return 0;
        id = id * 16 + (uint32_t)d;
    }
    return id;
}
