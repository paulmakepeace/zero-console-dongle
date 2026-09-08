#pragma once
#include <Arduino.h>

// Quote-safe text for a JSON string value. Bytes that are not valid UTF-8
// (an SSID is opaque octets) are written as \u00XX so the document parses.
inline String jsonEscape(const String& in) {
    const uint8_t* b = (const uint8_t*)in.c_str();
    size_t n = in.length();
    bool utf8 = true;
    for (size_t i = 0; i < n && utf8;) {
        uint8_t c = b[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > n) { utf8 = false; break; }
        for (size_t k = 1; k < len; k++) if ((b[i + k] & 0xC0) != 0x80) { utf8 = false; break; }
        i += len;
    }
    String out;
    out.reserve(n + 8);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20 || (!utf8 && c >= 0x80)) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); out += t; }
        else out += (char)c;
    }
    return out;
}
