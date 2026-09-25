#pragma once
/* LZ40 block decompression for the Dreamcast (CUE's LZX low-endian "LZ40",
 * `-ewl` WRAM mode, header 0x40 + 24-bit LE decompressed length).
 *
 * Decoder: https://github.com/VincentNLOBJ/LZ40_Decompress_SH4
 * (SH-4 ASM, MIT (c) 2024 VincentNL; original LZX/LZ40 format by CUE,
 * https://www.romhacking.net/utilities/826/, GPLv3). The Dreamcast build
 * runs the ASM itself (lz40_dec.S, GAS port of the vendored LZ40_dec.asm);
 * host tools and other targets use the portable C port below (same byte
 * format, no extra tables, floats, mallocs or unaligned word loads).
 *
 * Layout: [0x40][len lo][len mid][len hi] then flag bytes (stored negated:
 * logical = (uint8_t)(0 - stored), 1 = match, 0 = literal, MSB first) and
 * symbols:
 *   literal: 1 byte.
 *   match: 2 bytes LE (pos = b0 | b1<<8, low nibble = len bits):
 *     nibble > 1:  copy nibble bytes from -((pos>>4))            (2..15, 2 bytes)
 *     nibble == 0: +1 byte ab: copy (ab + 0x10) bytes            (16..271, 3 bytes)
 *     nibble == 1: +2 bytes (cd ab LE): copy (abcd + 0x110) bytes (272.., 4 bytes)
 *   offset (pos>>4) is 1..4095 (WRAM allows 1); 0 only appears in the
 *   encoder's terminator after the payload, which decoding never reaches.
 *
 * lz40_decode returns the decompressed size (== declen from the header) or
 * < 0 on malformed input. It stops at declen: callers may pass padding
 * after the stream (pack blocks are padded to 32 bytes, DCMV frames to 32).
 * NOTE (Dreamcast/ASM): the header and declen <= dstcap are validated in C;
 * the ASM fast loop itself trusts the stream (no input bound or offset
 * checks), exactly like upstream.
 */
#include <stdint.h>

#if defined(PLAT_DREAMCAST) || defined(__sh__)
/* SH-4 ASM core (lz40_dec.S): r4 = dst, r5 = src at the 0x40 magic. */
int LZ40_decompress(uint8_t *dst, const uint8_t *src);
#endif

static inline int lz40_decode_c(const uint8_t *src, int srcsize, uint8_t *dst, int dstcap)
{
    const uint8_t *ip, *iend;
    uint8_t *op, *oend;
    uint32_t declen;
    unsigned flags = 0, mask = 0;

    if (!src || !dst || srcsize < 4 || dstcap < 0) return -1;
    if (src[0] != 0x40) return -1;
    declen = (uint32_t)src[1] | ((uint32_t)src[2] << 8) | ((uint32_t)src[3] << 16);
    if (declen > (uint32_t)dstcap) return -1;

    ip = src + 4;
    iend = src + srcsize;
    op = dst;
    oend = dst + declen;

    while (op < oend) {
        unsigned bit;
        uint32_t pos;
        unsigned tmp;
        uint32_t len = 0;

        mask >>= 1;
        if (mask == 0) {
            unsigned stored;
            if (ip >= iend) return -1;
            stored = *ip++;
            flags = (unsigned)((0 - (int)stored) & 0xFF);   /* CUE: flags = -stored */
            mask = 0x80;
        }
        bit = flags & mask;
        if (!bit) {
            if (ip >= iend) return -1;
            *op++ = *ip++;
            continue;
        }
        if (ip + 2 > iend) return -1;
        pos = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
        ip += 2;
        tmp = pos & 0xF;
        if (tmp >= 2) {
            len = tmp;
            pos >>= 4;
        } else {
            uint32_t threshold;
            if (ip >= iend) return -1;
            len = *ip++;
            threshold = 0x10;
            if (tmp == 1) {
                if (ip >= iend) return -1;
                len |= (uint32_t)*ip++ << 8;
                threshold = 0x110;
            }
            len += threshold;
            pos >>= 4;
        }
        if (pos == 0 || pos > (uint32_t)(op - dst)) return -1;
        if (len == 0 || (uint32_t)(oend - op) < len) return -1;
        {
            const uint8_t *m = op - pos;
            /* overlapping copy repeats the pattern (offset 1 = RLE) */
            while (len--) *op++ = *m++;
        }
    }
    return (int)(op - dst);
}

/* Validated entry point used by pack.c / dcfmv.c (Dreamcast FMV/packs).
 * On the SH-4 this checks the header and declen, then runs the ASM core;
 * everywhere else it runs the portable C port above. */
static inline int lz40_decode(const uint8_t *src, int srcsize, uint8_t *dst, int dstcap)
{
#if defined(PLAT_DREAMCAST) || defined(__sh__)
    uint32_t declen;
    if (!src || !dst || srcsize < 4 || dstcap < 0) return -1;
    if (src[0] != 0x40) return -1;
    declen = (uint32_t)src[1] | ((uint32_t)src[2] << 8) | ((uint32_t)src[3] << 16);
    if (declen > (uint32_t)dstcap) return -1;
    return LZ40_decompress(dst, src);
#else
    return lz40_decode_c(src, srcsize, dst, dstcap);
#endif
}
