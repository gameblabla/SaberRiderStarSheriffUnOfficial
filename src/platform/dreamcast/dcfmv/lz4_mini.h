#pragma once
/* LZ4 block decompression (the format of the DCMV packers' frames): enough of liblz4's API for dcfmv.c, which KOS
 * does not ship. Returns the decompressed size, or < 0 on malformed input. */
#include <stdint.h>
#include <string.h>

static inline int lz4_mini_decode(const uint8_t *src, int srcsize, uint8_t *dst, int cap, int want)
{
    /* srcsize < 0 (LZ4_decompress_fast): no input bound. Not src + 0x7fffffff: main RAM sits at 0x8C000000, so that
     * wraps below src and every frame would fail its first check. */
    const uint8_t *ip = src, *iend = srcsize >= 0 ? src + srcsize : (const uint8_t *)UINTPTR_MAX;
    uint8_t *op = dst, *oend = dst + cap;
    for (;;) {
        if (ip >= iend) return -1;
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) { unsigned b; do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255); }
        if (lit) {
            if (op + lit > oend || ip + lit > iend) return -1;
            memcpy(op, ip, lit); op += lit; ip += lit;
        }
        if ((want >= 0 && op - dst >= want) || ip >= iend) break;   /* the last sequence has literals only */
        if (ip + 2 > iend) return -1;
        size_t off = ip[0] | ip[1] << 8; ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return -1;
        size_t ml = (token & 15);
        if (ml == 15) { unsigned b; do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255); }
        ml += 4;
        if (op + ml > oend) return -1;
        const uint8_t *m = op - off;
        if (off >= ml) { memcpy(op, m, ml); op += ml; }
        else while (ml--) *op++ = *m++;   /* overlapping copy repeats the pattern */
    }
    return (int)(op - dst);
}
static inline int LZ4_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity)
{ return lz4_mini_decode((const uint8_t *)src, compressedSize, (uint8_t *)dst, dstCapacity, -1); }
static inline int LZ4_decompress_fast(const char *src, char *dst, int originalSize)
{ return lz4_mini_decode((const uint8_t *)src, -1, (uint8_t *)dst, originalSize, originalSize); }
