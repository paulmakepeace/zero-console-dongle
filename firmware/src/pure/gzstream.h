// A gzip stream on fixed arrays. Lines go in as they arrive and are
// compressed against the history of the file so far; the output buffer
// holds what has not yet reached the flash. A commit is a sync flush: the
// caller writes out[0..pending()) and calls taken(). The stream ends with
// the gzip trailer. Nothing here touches the heap.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "uzlib.h"

template <size_t HIST, size_t LINE_CAP, size_t OUT, unsigned HASH_BITS_>
struct GzStream {
    uint8_t win[HIST + LINE_CAP];
    size_t wpos = 0;
    const uint8_t* hash[1u << HASH_BITS_];
    uint8_t out[OUT];
    uzlib_comp c = {};
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t total = 0;
    bool started = false;

    static constexpr size_t OVERHEAD = 8 + 4 + 2;   // the largest flush or trailer the buffer must always have room for

    // A fresh stream: the gzip header lands in the output buffer.
    void begin() {
        memset(hash, 0, sizeof hash);
        memset(&c, 0, sizeof c);
        c.outbuf = out;
        c.outsize = OUT;
        c.hash_table = hash;
        c.hash_bits = HASH_BITS_;
        c.dict_size = HIST;
        wpos = 0;
        crc = 0xFFFFFFFFu;
        total = 0;
        static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        memcpy(out, hdr, 10);
        c.outlen = 10;
        started = false;
    }

    size_t pending() const { return (size_t)c.outlen; }
    bool overflowed() const { return c.overflow != 0; }

    // Room for a line of this length plus a flush afterwards, in the worst
    // case of no compression at all (fixed Huffman literals cost at most 9 bits).
    bool fits(size_t len) const { return pending() + len + len / 8 + 2 + OVERHEAD <= OUT; }

    // Append a line and its newline. Returns false, having done nothing,
    // when it would not fit: commit first.
    bool add(const char* line, size_t len) {
        if (len + 1 > LINE_CAP || !fits(len + 1)) return false;
        if (wpos + len + 1 > sizeof win) slide();
        if (!started) { zlib_start_block(&c); started = true; }
        uint8_t* p = win + wpos;
        memcpy(p, line, len);
        p[len] = '\n';
        uzlib_compress(&c, p, len + 1);
        crc = uzlib_crc32(p, len + 1, crc);
        total += len + 1;
        wpos += len + 1;
        return true;
    }

    // End the block so that everything so far decodes; the next add() opens a new one.
    void flush() {
        if (!started) return;
        zlib_sync_flush(&c);
        started = false;
    }

    void taken() { c.outlen = 0; }

    // The final block and the trailer.
    void finish() {
        if (started) { zlib_finish_stream(&c); started = false; }
        else {
            // No open block: a final empty stored block closes the stream.
            outbits(&c, 1, 1); outbits(&c, 0, 2);
            if (c.noutbits) outbits(&c, 0, 8 - c.noutbits);
            outbits(&c, 0x0000, 16); outbits(&c, 0xFFFF, 16);
        }
        uint32_t v = crc ^ 0xFFFFFFFFu;
        uint8_t t[8] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24),
                        (uint8_t)total, (uint8_t)(total >> 8), (uint8_t)(total >> 16), (uint8_t)(total >> 24)};
        for (uint8_t b : t) outbits(&c, b, 8);
    }

private:
    // Keep the last HIST bytes as history at the front; the hash entries
    // pointed into the old positions, so they are rebuilt for the history.
    void slide() {
        size_t keep = wpos < HIST ? wpos : HIST;
        memmove(win, win + wpos - keep, keep);
        wpos = keep;
        memset(hash, 0, sizeof hash);
        uzlib_hash_add(&c, win, keep);
    }
};
