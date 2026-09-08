// A zlib stream on fixed arrays with a preset dictionary. The window is
// [dictionary][history][line]: the dictionary stays put for the whole
// stream and the history slides behind it, so a line can match text from
// this file or from the dictionary alike. Lines go in as they arrive; the
// output buffer holds what has not yet reached the flash; a commit is a
// sync flush; the stream ends with the Adler-32 trailer. The header names
// the dictionary by its Adler-32, as zlib's FDICT does, so the reader can
// fetch the right one. Nothing here touches the heap.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "uzlib.h"

template <size_t DICT, size_t HIST, size_t LINE_CAP, size_t OUT, unsigned HASH_BITS_>
struct ZStream {
    static_assert(DICT + HIST + LINE_CAP <= 32768, "deflate distances reach 32 KB at most");
    uint8_t win[DICT + HIST + LINE_CAP];
    size_t dlen = 0;    // dictionary bytes at the front of win
    size_t wpos = 0;    // end of data in win
    const uint8_t* hash[1u << HASH_BITS_];
    uint8_t out[OUT];
    uzlib_comp c = {};
    uint32_t adler = 1;
    uint32_t dictId = 0;
    uint32_t total = 0;
    bool started = false;

    static constexpr size_t OVERHEAD = 8 + 4 + 2;   // the largest flush or trailer the buffer must always have room for

    // A fresh stream. The dictionary, if any, is copied to the front of the
    // window and hashed; the header names it.
    void begin(const uint8_t* dict, size_t dictLen) {
        memset(hash, 0, sizeof hash);
        memset(&c, 0, sizeof c);
        c.outbuf = out;
        c.outsize = OUT;
        c.hash_table = hash;
        c.hash_bits = HASH_BITS_;
        c.dict_size = 32768;        // deflate's reach; a dictionary further back than that is out of the decoder's window
        if (dictLen > DICT) dictLen = DICT;
        c.dict_end = dictLen ? win + dictLen : nullptr;
        c.dict_extra = 0;
        memcpy(win, dict, dictLen);
        dlen = wpos = dictLen;
        uzlib_hash_add(&c, win, dlen);
        adler = 1;
        total = 0;
        started = false;
        out[0] = 0x78;   // deflate, 32 KB window declared
        if (dlen) {
            dictId = uzlib_adler32(dict, dictLen, 1);
            out[1] = 0x20;   // FDICT, level bits 0, check bits 0: 0x7820 is divisible by 31
            out[2] = dictId >> 24; out[3] = dictId >> 16; out[4] = dictId >> 8; out[5] = dictId;
            c.outlen = 6;
        } else {
            dictId = 0;
            out[1] = 0x01;   // no dictionary: 0x7801 is divisible by 31
            c.outlen = 2;
        }
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
        adler = uzlib_adler32(p, len + 1, adler);
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

    // The final block and the Adler-32 trailer.
    void finish() {
        if (started) { zlib_finish_stream(&c); started = false; }
        else {
            outbits(&c, 1, 1); outbits(&c, 0, 2);   // a final empty stored block
            if (c.noutbits) outbits(&c, 0, 8 - c.noutbits);
            outbits(&c, 0x0000, 16); outbits(&c, 0xFFFF, 16);
        }
        uint8_t t[4] = {(uint8_t)(adler >> 24), (uint8_t)(adler >> 16), (uint8_t)(adler >> 8), (uint8_t)adler};
        for (uint8_t b : t) outbits(&c, b, 8);
    }

    // Scratch space for the caller between streams: the history region,
    // free once a stream has ended.
    uint8_t* scratch() { return win + DICT; }
    static constexpr size_t SCRATCH = HIST + LINE_CAP;

private:
    // Keep the dictionary and the last HIST bytes of history; the hash
    // entries pointed into old positions, so the table is rebuilt.
    void slide() {
        size_t have = wpos - dlen;
        size_t keep = have < HIST ? have : HIST;
        c.dict_extra += have - keep;   // the bytes dropped still separate the dictionary from the data in the stream
        memmove(win + dlen, win + wpos - keep, keep);
        wpos = dlen + keep;
        memset(hash, 0, sizeof hash);
        uzlib_hash_add(&c, win, dlen);            // the two regions hashed apart: no key straddles the boundary
        uzlib_hash_add(&c, win + dlen, keep);
    }
};
