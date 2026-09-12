// The push URL: http://host[:port][/path]. Only http, on the LAN: TLS is
// not carried. A trailing slash on the path is dropped, so the file's
// path can always be appended as /board/name.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

struct PushUrl {
    char host[64];
    uint16_t port;
    char path[64];   // "" or "/segment[/segment]"
};

inline bool parsePushUrl(const char* s, size_t len, PushUrl& out) {
    static const char scheme[] = "http://";
    const size_t sl = sizeof(scheme) - 1;
    if (len <= sl || memcmp(s, scheme, sl) != 0) return false;
    size_t i = sl, h = i;
    while (i < len && s[i] != ':' && s[i] != '/') i++;
    size_t hl = i - h;
    if (hl == 0 || hl >= sizeof out.host) return false;
    memcpy(out.host, s + h, hl);
    out.host[hl] = 0;
    out.port = 80;
    if (i < len && s[i] == ':') {
        i++;
        unsigned p = 0;
        size_t d = 0;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, d++) { p = p * 10 + (s[i] - '0'); if (p > 65535) return false; }
        if (d == 0 || p == 0) return false;
        out.port = (uint16_t)p;
    }
    size_t pl = len - i;
    while (pl > 0 && s[i + pl - 1] == '/') pl--;
    if (pl >= sizeof out.path) return false;
    for (size_t k = 0; k < pl; k++) {
        char c = s[i + k];
        if (c <= ' ' || c == '?' || c == '#' || c == '"' || c == '\\') return false;
    }
    if (pl && s[i] != '/') return false;
    memcpy(out.path, s + i, pl);
    out.path[pl] = 0;
    return true;
}
