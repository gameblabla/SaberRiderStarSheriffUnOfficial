#include "sv24_frame_v03.h"

static uint16_t r16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t r32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int sv24_v03_frame_parse(const uint8_t *f, size_t bytes, sv24_v03_frame_t *o)
{
    uint32_t declared;
    unsigned ns, i, changed = 0u;
    uint32_t covered = 0u;
    const uint8_t *d, *p, *end;

    if (!f || !o || bytes < 26u || r32(f) != 0x46524d33u) return -1;
    declared = r32(f + 4);
    if (declared < 26u || declared != bytes) return -2;
    ns = f[18];
    if (!ns || ns > SV24_V03_MAX_SLICES || 26u + ns * 8u > declared) return -3;

    o->frame_no = r32(f + 8);
    o->pts = r32(f + 12);
    o->flags = r16(f + 16);
    o->slice_count = (uint8_t)ns;
    o->coding_flags = f[19];
    o->changed_tiles = r16(f + 20);
    o->schedule_mask = r16(f + 22);

    d = f + 26;
    p = d + ns * 8u;
    end = f + declared;
    for (i = 0u; i < ns; i++) {
        unsigned y0 = r16(d + i * 8u);
        unsigned rows = d[i * 8u + 2u];
        uint32_t sz = r32(d + i * 8u + 4u);
        unsigned tc, cc, mb;
        if (!rows || y0 + rows > 30u || sz < 4u || (size_t)(end - p) < sz)
            return -4;
        /* Dual SH-2 jobs must never overlap or race on the same cells. */
        uint32_t mask = ((1u << rows) - 1u) << y0;
        if (covered & mask) return -7;
        covered |= mask;
        tc = r16(p);
        cc = r16(p + 2);
        mb = (tc + 7u) >> 3;
        if (tc != rows * 44u || cc > tc || sz < 4u + mb) return -5;
        unsigned set = 0u;
        for (unsigned t = 0; t < tc; t++) set += (p[4u + (t >> 3)] >> (7u - (t & 7u))) & 1u;
        if (set != cc) return -8;
        o->slice[i].data = p;
        o->slice[i].size = sz;
        o->slice[i].tile_y0 = (uint16_t)y0;
        o->slice[i].tile_rows = (uint8_t)rows;
        o->slice[i].work_hint = d[i * 8u + 3u];
        changed += cc;
        p += sz;
    }
    if (p != end || changed != o->changed_tiles) return -6;
    return 0;
}
