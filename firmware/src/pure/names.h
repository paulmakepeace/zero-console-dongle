// Log file names: what the HTTP side accepts, and how a session is named.
#pragma once
#include <cstddef>
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

// bBBBB-SSS-<when>.log: boot count and sequence first so names sort by creation.
inline int sessionName(char* out, size_t cap, unsigned long boot, int seq, const char* when) {
    return snprintf(out, cap, "b%04lu-%03d-%s.log", boot, seq, when);
}
