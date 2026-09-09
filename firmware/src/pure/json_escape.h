// Quote-safe text for a JSON string value. Bytes that are not valid UTF-8
// (an SSID is opaque octets) are written as \u00XX so the document parses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

inline bool validUtf8(const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = b[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > n) return false;
        for (size_t k = 1; k < len; k++) if ((b[i + k] & 0xC0) != 0x80) return false;
        // The length alone lets through what UTF-8 forbids: an overlong
        // encoding, a surrogate half, and anything past the last code point.
        uint32_t cp = len == 1 ? c
                    : len == 2 ? (uint32_t)(c & 0x1F) << 6 | (b[i + 1] & 0x3F)
                    : len == 3 ? (uint32_t)(c & 0x0F) << 12 | (uint32_t)(b[i + 1] & 0x3F) << 6 | (b[i + 2] & 0x3F)
                               : (uint32_t)(c & 0x07) << 18 | (uint32_t)(b[i + 1] & 0x3F) << 12 |
                                 (uint32_t)(b[i + 2] & 0x3F) << 6 | (b[i + 3] & 0x3F);
        static const uint32_t least[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < least[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

inline std::string jsonEscape(const char* in, size_t n) {
    const uint8_t* b = (const uint8_t*)in;
    bool utf8 = validUtf8(b, n);
    std::string out;
    out.reserve(n + 8);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20 || (!utf8 && c >= 0x80)) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); out += t; }
        else out += (char)c;
    }
    return out;
}
