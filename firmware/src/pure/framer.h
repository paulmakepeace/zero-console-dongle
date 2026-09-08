// Bytes to lines: NUL and CR are dropped, LF ends a line, and a line that
// would overflow the buffer goes out early with its tail replaced by a
// marker so the reader knows it continues.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

template <size_t N>
struct LineFramer {
    static constexpr const char* CONTINUES = " [dongle: line continues]";
    char line[N];
    size_t len = 0;

    template <class Emit>
    void feed(const uint8_t* data, size_t n, Emit&& emit) {
        for (size_t i = 0; i < n; i++) {
            uint8_t b = data[i];
            if (b == 0 || b == '\r') continue;
            if (b == '\n') { emit(line, len); len = 0; continue; }
            if (len >= N - 1) {
                const size_t m = strlen(CONTINUES);
                memcpy(line + len - m, CONTINUES, m);
                emit(line, len);
                len = 0;
            }
            line[len++] = (char)b;
        }
    }

    // The partial line, for a prompt or a timeout; false when there is none.
    template <class Emit>
    bool flush(Emit&& emit) {
        if (!len) return false;
        emit(line, len);
        len = 0;
        return true;
    }
};
