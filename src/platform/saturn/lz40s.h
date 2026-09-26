#pragma once
/* LZ40S, the Saturn's LZ40: CUE's LZ40 scheme (the Dreamcast's, src/platform/dreamcast/dcfmv/lz40.h) in a big-endian
 * byte layout made for the SH-2 decoder in lz40s.sx. The format is in tools/dc/lz40.py (lz40s_compress):
 *   [0x41][declen, 24-bit big-endian], flag bytes (1 = match, LSB first), literals and matches, then an end match.
 * lz40s_decode returns the decoded size (the header's) or -1: a bad header, no room, or (C) a malformed stream.
 * The SH-2 core trusts the stream past the header check, as the Dreamcast's does; lz40s_decode_c is the checked
 * portable version (host tools, tests). Pack blocks and texture parts are padded after the stream: harmless. */
#include <stdint.h>

#ifdef PLAT_SATURN
uint8_t *lz40s_run(uint8_t *dst, const uint8_t *stream);   /* lz40s.sx: the end of the output */
#endif

static inline int lz40s_decode_c(const uint8_t *src, int srcsize, uint8_t *dst, int dstcap)
{
    if (!src || !dst || srcsize < 4 || src[0] != 0x41) return -1;
    uint32_t declen = (uint32_t)src[1] << 16 | (uint32_t)src[2] << 8 | src[3];
    if (dstcap < 0 || declen > (uint32_t)dstcap) return -1;
    const uint8_t *ip = src + 4, *iend = src + srcsize;
    uint8_t *op = dst, *oend = dst + declen;
    unsigned flags = 1;
    for (;;) {
        if (flags == 1) { if (ip >= iend) return -1; flags = *ip++ | 0x100u; }
        unsigned f = flags & 1; flags >>= 1;
        if (!f) { if (ip >= iend || op >= oend) return -1; *op++ = *ip++; continue; }
        if (ip + 2 > iend) return -1;
        unsigned b0 = ip[0]; int32_t m = (int32_t)((b0 & 15u) << 8) + (int8_t)ip[1]; ip += 2;
        if (m < 0) break;
        uint32_t len = b0 >> 4;
        if (len == 0) { if (ip >= iend) return -1; len = *ip++ + 16u; }
        else if (len == 1) { if (ip + 2 > iend) return -1; len = (uint32_t)ip[0] << 8 | ip[1]; ip += 2; }
        if ((uint32_t)m >= (uint32_t)(op - dst) || len > (uint32_t)(oend - op)) return -1;
        const uint8_t *s = op - m - 1;
        while (len--) *op++ = *s++;
    }
    return op == oend ? (int)declen : -1;
}

static inline int lz40s_decode(const uint8_t *src, int srcsize, uint8_t *dst, int dstcap)
{
#ifdef PLAT_SATURN
    if (!src || !dst || srcsize < 4 || src[0] != 0x41) return -1;
    uint32_t declen = (uint32_t)src[1] << 16 | (uint32_t)src[2] << 8 | src[3];
    if (dstcap < 0 || declen > (uint32_t)dstcap) return -1;
    return lz40s_run(dst, src + 4) == dst + declen ? (int)declen : -1;
#else
    return lz40s_decode_c(src, srcsize, dst, dstcap);
#endif
}
