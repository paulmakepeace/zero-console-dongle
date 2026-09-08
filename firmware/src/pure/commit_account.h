// What a commit to the flash actually kept. The stdio buffer accepts writes a
// full filesystem cannot hold, so the bytes lost at the flush are the tail of
// what was written, and the loss is counted once, from the tail of the
// buffer that did not reach the flash.
#pragma once
#include <cstddef>

struct CommitAccount {
    size_t kept;          // bytes of the buffer that are on the flash
    size_t lostAtFlush;   // bytes the file grew in the buffer but not on the flash
};

inline CommitAccount commitAccount(size_t written, size_t sizeAfterWrite, size_t sizeOnDisk) {
    size_t lost = sizeOnDisk < sizeAfterWrite ? sizeAfterWrite - sizeOnDisk : 0;
    return {written > lost ? written - lost : 0, lost};
}

inline size_t countLines(const char* s, size_t n) {
    size_t lines = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') lines++;
    return lines;
}
