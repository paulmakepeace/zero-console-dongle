// Small parsers over the MBB's command output: the prompt, the pack's state
// of charge from `bms`, the bike state from `state` or `status`.
#pragma once
#include <cstddef>
#include <cstring>
#include <cctype>

inline bool isPrompt(const char* s, size_t len) {
    static const char p[] = "ZERO MBB>";
    return len >= sizeof(p) - 1 && memcmp(s, p, sizeof(p) - 1) == 0;
}

// Lines the MBB prints on its own, which belong in the log even when they
// land in the middle of a command's output.
inline bool isUnsolicited(const char* s, size_t len) {
    static const char* const heads[] = {"DEBUG:", "INFO:", "Fault set:", "Fault cleared:", "Charge State ",
                                        "Disch limits:", "Ch limits:", "Saving Stats", "Reset Source", "blinker current"};
    for (const char* h : heads) {
        size_t n = strlen(h);
        if (len >= n && memcmp(s, h, n) == 0) return true;
    }
    static const char sc[] = "State change from";
    for (size_t i = 0; i + sizeof(sc) - 1 <= len; i++) if (memcmp(s + i, sc, sizeof(sc) - 1) == 0) return true;
    return false;
}

// The integer after `key` on the first line that starts with it after
// leading spaces and dashes; -1 when absent.
inline long numberAfter(const char* text, size_t len, const char* key) {
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && (text[j] == ' ' || text[j] == '-')) j++;
        if (j + klen <= len && memcmp(text + j, key, klen) == 0) {
            j += klen;
            while (j < len && text[j] == ' ') j++;
            if (j < len && isdigit((unsigned char)text[j])) {
                long v = 0;
                while (j < len && isdigit((unsigned char)text[j])) { v = v * 10 + (text[j] - '0'); j++; }
                return v;
            }
        }
        while (i < len && text[i] != '\n') i++;
        i++;
    }
    return -1;
}

inline long parseSoc(const char* bmsText, size_t len) { return numberAfter(bmsText, len, "soc"); }

// "Bike State: CHRG" -> "CHRG". Returns the length written, 0 when absent.
inline size_t parseBikeState(const char* text, size_t len, char* out, size_t cap) {
    static const char key[] = "Bike State:";
    for (size_t i = 0; i + sizeof(key) - 1 <= len; i++) {
        if (memcmp(text + i, key, sizeof(key) - 1) != 0) continue;
        size_t j = i + sizeof(key) - 1;
        while (j < len && text[j] == ' ') j++;
        size_t k = 0;
        while (j < len && k + 1 < cap && text[j] != '\n' && text[j] != '\r' && text[j] != ' ') out[k++] = text[j++];
        out[k] = 0;
        return k;
    }
    if (cap) out[0] = 0;
    return 0;
}
