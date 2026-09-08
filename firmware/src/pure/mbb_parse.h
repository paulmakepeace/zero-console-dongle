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
    // A line that opens with the MBB's own stamp is narration; a command's output never does.
    static const char pat[] = "dd/dd/dddd dd:dd:dd.ddd";
    if (len >= sizeof(pat) - 1) {
        bool m = true;
        for (size_t j = 0; j < sizeof(pat) - 1 && m; j++) {
            char c = s[j];
            m = pat[j] == 'd' ? (c >= '0' && c <= '9') : (c == pat[j]);
        }
        if (m) return true;
    }
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

// The BMS row of `status`:
//  BMS | SOC |  Pack V  | Current | Capacity|  L cell  | H temp | L temp | Cont | Elig
//    2   86 %  108555 mV  -12284 mA     84 AH    3873 mV    31 C    29 C      +     + +
struct PackRow { long soc, packMv, currentMa, capacityAh, lowCellMv, tempHiC, tempLoC; };

inline bool parsePackRow(const char* text, size_t len, PackRow& out) {
    static const char key[] = "BMS | SOC";
    size_t i = 0;
    for (; i + sizeof(key) - 1 <= len; i++) if (memcmp(text + i, key, sizeof(key) - 1) == 0) break;
    if (i + sizeof(key) - 1 > len) return false;
    while (i < len && text[i] != '\n') i++;   // to the end of the header
    // The row is the next line with a digit in it; a separator of dashes may sit between.
    for (int tries = 0; tries < 3; tries++) {
        size_t j = i + 1;
        bool digit = false;
        while (j < len && text[j] != '\n') { if (isdigit((unsigned char)text[j])) digit = true; j++; }
        if (digit) break;
        if (j >= len) return false;
        i = j;
    }
    long v[8];
    int n = 0;
    bool neg = false, inNum = false;
    long cur = 0;
    for (i++; i < len && text[i] != '\n' && n < 8; i++) {
        char c = text[i];
        if (c == '-' && !inNum) { neg = true; continue; }
        if (isdigit((unsigned char)c)) { inNum = true; cur = cur * 10 + (c - '0'); continue; }
        if (inNum) { v[n++] = neg ? -cur : cur; cur = 0; inNum = false; }
        neg = false;
    }
    if (inNum && n < 8) v[n++] = neg ? -cur : cur;
    if (n < 8) return false;   // bms id, soc, mv, ma, ah, cell mv, hi c, lo c
    out = {v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
    return true;
}

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
