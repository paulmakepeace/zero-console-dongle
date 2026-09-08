/*
 * uzlib - tiny deflate/inflate library (deflate, gzip, zlib)
 *
 * Copyright (c) 2003 by Joergen Ibsen / Jibz
 * Copyright (c) 2014-2018 by Paul Sokolovsky
 *
 * The compression side only, for the dongle: see NOTES.
 */
#ifndef UZLIB_H_INCLUDED
#define UZLIB_H_INCLUDED
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef const uint8_t *uzlib_hash_entry_t;

struct uzlib_comp {
    unsigned char *outbuf;
    int outlen, outsize;
    int overflow;
    unsigned long outbits;
    int noutbits;
    int comp_disabled;

    uzlib_hash_entry_t *hash_table;
    unsigned int hash_bits;
    unsigned int dict_size;
};

void uzlib_compress(struct uzlib_comp *c, const uint8_t *src, unsigned slen);
void uzlib_hash_add(struct uzlib_comp *c, const uint8_t *src, unsigned slen);
uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t prev_sum);

#include "defl_static.h"

#ifdef __cplusplus
}
#endif
#endif
