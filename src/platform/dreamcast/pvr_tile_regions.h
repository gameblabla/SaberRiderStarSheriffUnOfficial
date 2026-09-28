#pragma once
#include <stdint.h>

/* Alpha map entries describe 8x8 texel blocks. A 16x16 tile can contain four
 * different classes even when the whole tile was classified as translucent. */
enum { CLS_OPAQUE, CLS_CUT, CLS_TRANS, CLS_EMPTY };
#define AM_NOT_OPAQUE 1
#define AM_NOT_EMPTY  2
#define AM_PARTIAL    4

static inline int am_class(unsigned m)
{
    return !(m & AM_NOT_OPAQUE) ? CLS_OPAQUE : !(m & AM_NOT_EMPTY) ? CLS_EMPTY : !(m & AM_PARTIAL) ? CLS_CUT : CLS_TRANS;
}

/* Coordinates in 8-texel units. Merge equal neighbours so uniform tiles still
 * cost one quad. Include empty regions here; the blend mode decides whether
 * they may be omitted (a plain copy must preserve them). No pixel is duplicated. */
typedef struct { uint8_t x, y, w, h, cls; } PvrTileRegion;
static inline int pvr_tile_regions(const uint8_t cls[4], PvrTileRegion out[4])
{
    unsigned used = 0;
    int n = 0;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
        int i = y * 2 + x;
        if (used & (1u << i)) continue;
        int w = x == 0 && cls[i] == cls[i + 1] && !(used & (1u << (i + 1))) ? 2 : 1;
        int h = y == 0 && cls[i] == cls[i + 2] && (w == 1 || cls[i] == cls[i + 3]) ? 2 : 1;
        for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) used |= 1u << (i + dy * 2 + dx);
        out[n++] = (PvrTileRegion){ x, y, w, h, cls[i] };
    }
    return n;
}
