/* platform/render.h on the Saturn: every draw is a VDP1 command (plan 4.4).
 *
 * Textures are "SAT1" blocks (tools/saturn/satbake.py): the rectangles the game draws out of a texture ("units": a
 * sprite frame, a tile, a glyph) are stored as VDP1-ready "parts" (4bpp with a colour lookup table, 8bpp indices into
 * the texture's palette, or 16bpp RGB),
 * LZ4-compressed. A texture keeps its block in work RAM (low RAM for big ones); a part is decoded into video memory the
 * first time it is drawn and stays there until the space is needed (least recently used first; never a part drawn in
 * this frame or the one VDP1 may still be drawing). Runtime RGBA textures (rtex_create) are turned into such a block.
 *
 * The frame's commands are built in high work RAM and DMA'd to VDP1 at rsat_frame_end. The framebuffer mixes RGB pixels
 * (bit 15 set: 4bpp colour tables and 16bpp parts) and palette pixels (8bpp parts: VDP1 colour-bank mode, the colour
 * code pointing into VDP2 colour RAM, where each 8bpp texture's palette gets a 64/128/256-entry bank while it is drawn).
 * VDP1 has no general alpha or colour multiply:
 *   alpha:  < 32 not drawn, < 96 mesh + half-transparency (~25 %), < 192 half-transparency, else opaque;
 *           palette pixels can't be blended by VDP1: an 8bpp part uses mesh instead, and so does a translucent
 *           polygon once palette pixels may be under it (half-transparency over them draws opaque);
 *   fades:  a translucent full-screen fill that ends the frame is the VDP2 colour offset instead (plan 4.4);
 *   add:    half-transparency (VDP2 colour calculation comes with the palette sprites, plan 4.4);
 *   colour mod: gouraud shading, which adds or subtracts per channel (an approximation of the multiply); an 8bpp
 *           texture gets a tinted copy of its palette instead (exact). */
#include "../render.h"
#include "../plat.h"
#include "sat_internal.h"
#include "../dreamcast/dcfmv/lz4_mini.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------- VDP1 memory */
#define VDP1_VRAM_BASE   0x25C00000u
#define CMD_MAX          2000                   /* commands a frame (64 KB of VRAM reserved) */
#define GOURAUD_OFF      0x10000u               /* 512 gouraud tables of 8 bytes */
#define GOURAUD_MAX      512
#define TEX_OFF          0x11000u
#define TEX_END          0x80000u
#define STAGING_SIZE     (32 * 1024)            /* satbake.py MAX_RAW */

typedef struct __attribute__((packed)) { uint16_t ctrl, link, pmod, colr, srca, size; int16_t xa, ya, xb, yb, xc, yc, xd, yd; uint16_t grda, dummy; } Cmd;

enum { C_NORMAL = 0, C_SCALED = 1, C_DISTORTED = 2, C_POLYGON = 4, C_POLYLINE = 5, C_LINE = 6, C_USER_CLIP = 8, C_SYS_CLIP = 9, C_LOCAL = 10, C_END = 0x8000 };
enum { PM_ECD = 0x80, PM_SPD = 0x40, PM_MESH = 0x100, PM_CLIP = 0x400, PM_PCLP = 0x800,
       PM_LUT = 1 << 3, PM_RGB = 5 << 3, PM_HALF = 3, PM_GOURAUD = 4 };
enum { FMT_4BPP = 0, FMT_16BPP = 1, FMT_8BPP = 2 };

/* ---------------------------------------------------------------- textures */
typedef struct { uint16_t x, y, w, h, first, n; } Unit;
typedef struct { uint16_t x, y, w, h, wpad; uint8_t fmt, pad; uint32_t off, len; } Part;

struct Ren { int prims; };
struct RTex {
    Ren *r;
    int w, h;
    uint8_t *block; size_t size;
    const Unit *units; int nunits;
    const Part *parts; int nparts;
    uint16_t *loc;              /* per part: its VDP1 address / 8, 0 while not in video memory */
    uint32_t *used;             /* per part: the last frame it was drawn in (the cache's LRU; no search on a hit) */
    uint16_t *grid; int gw, gh, gcols;   /* units all gw x gh on a grid: unit index + 1 per grid cell (a direct lookup) */
    uint32_t gw_inv, gh_inv;             /* 2^20 / gw, rounded up: x / gw as a multiply (no libgcc division) */
    const uint8_t *pal; int npal; /* the 8bpp parts' palette (big-endian RGB555 of indices 1..npal) */
    uint8_t mr, mg, mb, alpha; RBlend blend; uint32_t tag;
    uint8_t prio;               /* 8bpp parts: sprite priority register (their colour code's bits 14-12) */
    bool owned_block;           /* built here (runtime texture) */
};
struct RFloor { int unused; };

static Ren ren;
Ren *rsat_renderer(void) { return &ren; }
int  rsat_prims(void) { return ren.prims; }

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---------------------------------------------------------------- the video memory cache */
typedef struct { uint32_t off, size; RTex *t; uint16_t part; uint32_t used; } Slot;   /* sorted by off; used: once orphaned */
#define MAX_SLOTS 1536
static Slot slots[MAX_SLOTS]; static int nslots;
static uint32_t frame_no = 2;
static uint8_t *staging;
static unsigned uploads_frame, upload_bytes_frame, evictions, cram_uploads;
static uint32_t tm_planes, tm_wait, tm_put, tm_frames, tm_tex, tm_ntex, tm_upl;   /* SABER_PERF timing (rsat_timing) */
static int trace_from, traced; static bool tracing;   /* SABER_RTRACE=n: every draw of frames n, n+10, ... n+50 */

/* when a slot's part was last drawn: its texture keeps it (an orphaned slot, its own copy) */
static uint32_t slot_used(int i) { return slots[i].t ? slots[i].t->used[slots[i].part] : slots[i].used; }

static int slot_find(uint32_t off)
{
    int lo = 0, hi = nslots - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (slots[m].off == off) return m; if (slots[m].off < off) lo = m + 1; else hi = m - 1; }
    return -1;
}
static void slot_remove(int i)
{
    Slot *s = &slots[i];
    if (s->t) s->t->loc[s->part] = 0;
    memmove(&slots[i], &slots[i + 1], (size_t)(nslots - i - 1) * sizeof *slots);
    nslots--;
}
/* first fit; 32-byte aligned (a colour table's rule) */
static int gap_fit(uint32_t size, uint32_t *off)
{
    uint32_t at = TEX_OFF;
    for (int i = 0; i <= nslots; i++) {
        uint32_t end = i < nslots ? slots[i].off : TEX_END;
        if (end >= at && end - at >= size) { *off = at; return i; }
        if (i < nslots) at = (slots[i].off + slots[i].size + 31) & ~31u;
    }
    return -1;
}
static bool vram_alloc(uint32_t size, RTex *t, int part, uint32_t *off)
{
    size = (size + 31) & ~31u;
    for (;;) {
        int at = nslots < MAX_SLOTS ? gap_fit(size, off) : -1;
        if (at >= 0) {
            memmove(&slots[at + 1], &slots[at], (size_t)(nslots - at) * sizeof *slots);
            slots[at] = (Slot){ *off, size, t, (uint16_t)part, frame_no };
            nslots++;
            return true;
        }
        /* evict the least recently used part that neither this frame nor the one being drawn needs */
        int victim = -1;
        for (int i = 0; i < nslots; i++)
            if (slot_used(i) + 1 < frame_no && (victim < 0 || slot_used(i) < slot_used(victim))) victim = i;
        if (victim < 0) return false;
        slot_remove(victim);
        evictions++;
    }
}

/* what the cache holds, per texture (debug: printed once when an allocation fails) */
static void vram_dump(void)
{
    uint32_t used = 0, gaps = 0, at = TEX_OFF;
    for (int i = 0; i < nslots; i++) { used += slots[i].size; gaps += slots[i].off - at; at = slots[i].off + slots[i].size; }
    gaps += TEX_END - at;
    printf("  vram: %u KB in parts, %u KB free in gaps, frame %u, %u palette uploads\n", (unsigned)(used / 1024), (unsigned)(gaps / 1024),
           (unsigned)frame_no, cram_uploads);
    for (int i = 0; i < nslots; i++) {
        RTex *t = slots[i].t;
        bool seen = false;
        for (int k = 0; k < i && !seen; k++) seen = slots[k].t == t;
        if (seen) continue;
        uint32_t bytes = 0, newest = 0; int n = 0;
        for (int k = i; k < nslots; k++)
            if (slots[k].t == t) { bytes += slots[k].size; n++; if (slot_used(k) > newest) newest = slot_used(k); }
        printf("    %08X %dx%d: %d parts %u KB, last used frame %u\n", t ? (unsigned)t->tag : 0u, t ? t->w : 0, t ? t->h : 0,
               n, (unsigned)(bytes / 1024), (unsigned)newest);
    }
}

static void vram_copy(uint32_t off, const uint8_t *src, uint32_t n)
{
    volatile uint32_t *d = (volatile uint32_t *)(VDP1_VRAM_BASE + off);
    const uint32_t *s = (const uint32_t *)src;
    for (uint32_t i = 0; i < n / 4; i++) d[i] = s[i];
}

static uint32_t part_raw_size(const Part *p)
{
    switch (p->fmt & 0x7F) {
    case FMT_4BPP: return 32u + (uint32_t)p->wpad * p->h / 2;
    case FMT_8BPP: return (uint32_t)p->wpad * p->h;
    default:       return (uint32_t)p->wpad * p->h * 2;
    }
}

/* ---------------------------------------------------------------- colour RAM: the 8bpp textures' palettes
 * CRAM mode 1 (2048 RGB555 entries) in 32 granules of 64; a palette takes 1, 2 or 4 aligned granules (VDP1's 64-, 128-
 * and 256-colour bank modes). A bank stays until the space is needed and it was neither used in this frame, nor in the
 * one VDP1 is drawing, nor in the one on screen. */
#define CRAM_GRAN 32
typedef struct { RTex *t; uint32_t tint, used; uint8_t n; } CSlot;   /* at its first granule; n 0: free */
static CSlot cslots[CRAM_GRAN];
static int8_t cowner[CRAM_GRAN];   /* granule -> its bank's first granule, -1 free */

static int pal_granules(int npal) { return npal < 64 ? 1 : npal < 128 ? 2 : 4; }

static void cram_release(int g)
{
    for (int k = 0; k < cslots[g].n; k++) cowner[g + k] = -1;
    cslots[g].n = 0; cslots[g].t = NULL;
}

/* colour RAM entries [0, n) go to the VDP2 planes (vdp2_planes.c); VDP1's banks there are dropped (a level's start) */
void rsat_cram_reserve(int entries)
{
    int n = (entries + 63) / 64;
    for (int g = 0; g < CRAM_GRAN; g++) {
        if (g < n) {
            if (cowner[g] >= 0) cram_release(cowner[g]);
            cowner[g] = -2;
        } else if (cowner[g] == -2) cowner[g] = -1;
    }
}

/* the colour code of t's palette tinted by (r, g, b) / 255, uploading it if needed; -1 if colour RAM is full */
static int cram_bank(RTex *t, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t tint = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    for (int i = 0; i < CRAM_GRAN; i++)
        if (cslots[i].n && cslots[i].t == t && cslots[i].tint == tint) { cslots[i].used = frame_no; return i * 64; }
    int need = pal_granules(t->npal);
    for (;;) {
        for (int base = 0; base < CRAM_GRAN; base += need) {
            bool free_run = true;
            for (int k = 0; k < need && free_run; k++) free_run = cowner[base + k] == -1;
            if (!free_run) continue;
            for (int k = 0; k < need; k++) cowner[base + k] = (int8_t)base;
            cslots[base] = (CSlot){ t, tint, frame_no, (uint8_t)need };
            volatile uint16_t *c = (volatile uint16_t *)VDP2_CRAM_ADDR(base * 64);
            c[0] = 0;
            for (int k = 0; k < t->npal; k++) {
                uint16_t v = be16(t->pal + 2 * k);
                if (tint != 0xFFFFFF) {
                    unsigned R = (v & 31) * r / 255, G = (v >> 5 & 31) * g / 255, B = (v >> 10 & 31) * b / 255;
                    v = (uint16_t)(B << 10 | G << 5 | R);
                }
                c[k + 1] = v;
            }
            cram_uploads++;
            return base * 64;
        }
        int victim = -1;
        for (int i = 0; i < CRAM_GRAN; i++)
            if (cslots[i].n && cslots[i].used + 2 < frame_no && (victim < 0 || cslots[i].used < cslots[victim].used)) victim = i;
        if (victim < 0) return -1;
        cram_release(victim);
    }
}

/* the part's address in video memory / 8 (uploading it if needed), 0 if there is no room */
static uint16_t part_resident(RTex *t, int i)
{
    const Part *p = &t->parts[i];
    t->used[i] = frame_no;
    if (t->loc[i]) return t->loc[i];
    uint32_t tu = sat_timer_us();
    uint32_t raw = part_raw_size(p), off;
    if (raw > STAGING_SIZE || !vram_alloc(raw, t, i, &off)) {
        static int warned;
        if (!warned++) {
            printf("render: no video memory for a %u byte part of %08X (%d parts resident)\n", (unsigned)raw, (unsigned)t->tag, nslots);
            vram_dump();
        }
        return 0;
    }
    const uint8_t *src = t->block + p->off;
    if (p->fmt & 0x80) {
        if (lz4_mini_decode(src, (int)p->len, staging, STAGING_SIZE, (int)raw) != (int)raw) {
            printf("render: texture %08X part %d: bad LZ4\n", (unsigned)t->tag, i);
            memset(staging, 0, raw);
        }
        src = staging;
    } else if ((uintptr_t)src & 3) { memcpy(staging, src, raw); src = staging; }
    vram_copy(off, src, raw);
    uploads_frame++; upload_bytes_frame += raw; tm_upl += sat_timer_us() - tu;
    t->loc[i] = (uint16_t)(off / 8);
    return t->loc[i];
}

/* a texture going away: its video memory and colour RAM are orphaned, not freed, so the frames VDP1 may still draw (or
 * show) keep them; the allocators reclaim them like any slot not used lately */
static void tex_free_vram(RTex *t)
{
    for (int i = 0; i < CRAM_GRAN; i++) if (cslots[i].n && cslots[i].t == t) cslots[i].t = NULL;
    for (int i = 0; i < t->nparts; i++)
        if (t->loc[i]) { int s = slot_find((uint32_t)t->loc[i] * 8); if (s >= 0) { slots[s].used = t->used[i]; slots[s].t = NULL; } t->loc[i] = 0; }
}

/* ---------------------------------------------------------------- texture objects */
static bool (*evict_hook)(void);
void r_set_evict_hook(bool (*hook)(void)) { evict_hook = hook; }

static void *alloc_retry(size_t n)
{
    void *p;
    while (!(p = malloc(n)))
        if (!evict_hook || !evict_hook()) return NULL;
    return p;
}

/* units that are all one size on a grid (sprite frames, tiles): a direct lookup table instead of a search */
static void build_grid(RTex *t)
{
    t->grid = NULL;
    if (t->nunits < 8) return;
    int gw = t->units[0].w, gh = t->units[0].h;
    if (!gw || !gh) return;
    for (int i = 0; i < t->nunits; i++)
        if (t->units[i].w != gw || t->units[i].h != gh || t->units[i].x % gw || t->units[i].y % gh) return;
    int cols = (t->w + gw - 1) / gw, rows = (t->h + gh - 1) / gh;
    uint16_t *g = calloc((size_t)cols * rows, sizeof *g);
    if (!g) return;
    for (int i = 0; i < t->nunits; i++) g[(t->units[i].y / gh) * cols + t->units[i].x / gw] = (uint16_t)(i + 1);
    t->grid = g; t->gw = gw; t->gh = gh; t->gcols = cols;
    t->gw_inv = ((1u << 20) + (uint32_t)gw - 1) / (uint32_t)gw; t->gh_inv = ((1u << 20) + (uint32_t)gh - 1) / (uint32_t)gh;
}

static RTex *tex_from_block(Ren *r, uint8_t *block, size_t size, bool owned)
{
    if (size < 32 || memcmp(block, "SAT1", 4)) return NULL;
    RTex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->r = r; t->w = le16(block + 4); t->h = le16(block + 6);
    t->nunits = le16(block + 8); t->nparts = le16(block + 10); t->npal = le16(block + 14);
    uint32_t uoff = le32(block + 24), poff = le32(block + 28);
    t->block = block; t->size = size; t->owned_block = owned;
    /* the tables into host structs (the block's are big-endian and packed) */
    Unit *u = malloc(sizeof(Unit) * (size_t)(t->nunits ? t->nunits : 1));
    Part *p = malloc(sizeof(Part) * (size_t)(t->nparts ? t->nparts : 1));
    t->loc = calloc((size_t)(t->nparts ? t->nparts : 1), sizeof *t->loc);
    t->used = calloc((size_t)(t->nparts ? t->nparts : 1), sizeof *t->used);
    if (!u || !p || !t->loc || !t->used || uoff + 12u * t->nunits > size || poff + 20u * t->nparts + 2u * t->npal > size || t->npal > 255) {
        free(u); free(p); free(t->loc); free(t->used); free(t); return NULL;
    }
    for (int i = 0; i < t->nunits; i++) {
        const uint8_t *d = block + uoff + 12 * i;
        u[i] = (Unit){ be16(d), be16(d + 2), be16(d + 4), be16(d + 6), be16(d + 8), be16(d + 10) };
    }
    for (int i = 0; i < t->nparts; i++) {
        const uint8_t *d = block + poff + 20 * i;
        p[i] = (Part){ be16(d), be16(d + 2), be16(d + 4), be16(d + 6), be16(d + 8), d[10], 0, be32(d + 12), be32(d + 16) };
    }
    t->units = u; t->parts = p; t->pal = block + poff + 20 * t->nparts;
    build_grid(t);
    t->mr = t->mg = t->mb = t->alpha = 255; t->blend = R_BLEND_BLEND;
    return t;
}

RTex *rtex_create_baked(Ren *r, uint8_t *block, size_t size)
{
    /* the block is the pack's buffer (released by the caller): keep a copy, in low work RAM when it is big */
    uint8_t *copy = alloc_retry(size);
    if (!copy) { printf("render: no RAM for a %u KB texture\n", (unsigned)(size / 1024)); return NULL; }
    memcpy(copy, block, size);
    RTex *t = tex_from_block(r, copy, size, true);
    if (!t) free(copy);
    return t;
}

/* a runtime RGBA texture as a SAT1 block: one unit, 16bpp bands of at most STAGING_SIZE bytes, stored raw */
static uint8_t *block_from_rgba(int w, int h, const uint32_t *px, int pitch_px, size_t *size_out)
{
    int wpad = (w + 7) & ~7, rows = STAGING_SIZE / (wpad * 2); if (rows > 255) rows = 255; if (rows < 1) rows = 1;
    int nparts = (h + rows - 1) / rows;
    size_t head = 32 + 12 + 20 * (size_t)nparts, data = (size_t)wpad * h * 2, size = head + data;
    uint8_t *b = calloc(1, size);
    if (!b) return NULL;
    memcpy(b, "SAT1", 4);
    b[4] = w & 255; b[5] = w >> 8; b[6] = h & 255; b[7] = h >> 8; b[8] = 1; b[10] = nparts & 255; b[11] = nparts >> 8;
    uint32_t uoff = 32, poff = 44;
    b[24] = uoff; b[28] = poff;
    uint8_t *u = b + uoff;
    u[4] = w >> 8; u[5] = w & 255; u[6] = h >> 8; u[7] = h & 255; u[10] = nparts >> 8; u[11] = nparts & 255;
    uint32_t off = (uint32_t)head;
    for (int k = 0; k < nparts; k++) {
        int y0 = k * rows, ph = h - y0 < rows ? h - y0 : rows;
        uint8_t *p = b + poff + 20 * k;
        p[2] = y0 >> 8; p[3] = y0 & 255; p[4] = w >> 8; p[5] = w & 255; p[6] = ph >> 8; p[7] = ph & 255; p[8] = wpad >> 8; p[9] = wpad & 255;
        p[10] = 1;
        uint32_t n = (uint32_t)wpad * ph * 2;
        p[12] = off >> 24; p[13] = off >> 16; p[14] = off >> 8; p[15] = off; p[16] = n >> 24; p[17] = n >> 16; p[18] = n >> 8; p[19] = n;
        uint16_t *o = (uint16_t *)(b + off);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < wpad; x++) {
                uint32_t c = x < w && px ? px[(size_t)(y0 + y) * pitch_px + x] : 0;   /* 0xAABBGGRR */
                o[y * wpad + x] = (c >> 31) ? (uint16_t)(0x8000 | ((c >> 19) & 31) << 10 | ((c >> 11) & 31) << 5 | ((c >> 3) & 31)) : 0;
            }
        off += n;
    }
    *size_out = size;
    return b;
}

RTex *rtex_create(Ren *r, int w, int h, RTexAccess a, const uint32_t *px)
{
    (void)a;
    size_t size; uint8_t *b = block_from_rgba(w, h, px, w, &size);
    RTex *t = b ? tex_from_block(r, b, size, true) : NULL;
    if (!t) free(b);
    return t;
}

RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud)
{
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    for (int y = 0; y < h; y += 16) rows(ud, y, h - y < 16 ? h - y : 16, px + (size_t)y * w);
    RTex *t = rtex_create(r, w, h, R_TEX_STATIC, px);
    free(px);
    return t;
}

void rtex_update(RTex *t, const uint32_t *px, int pitch_bytes)
{
    if (!t) return;
    size_t size; uint8_t *b = block_from_rgba(t->w, t->h, px, pitch_bytes / 4, &size);
    if (!b) return;
    RTex *n = tex_from_block(t->r, b, size, true);
    if (!n) { free(b); return; }
    tex_free_vram(t);
    free(t->block); free((void *)t->units); free((void *)t->parts); free(t->loc); free(t->used); free(t->grid);
    t->block = n->block; t->size = n->size; t->units = n->units; t->parts = n->parts; t->loc = n->loc; t->pal = n->pal; t->npal = n->npal;
    t->used = n->used; t->grid = n->grid; t->gw = n->gw; t->gh = n->gh; t->gcols = n->gcols;
    t->nunits = n->nunits; t->nparts = n->nparts;
    free(n);
}

void rtex_destroy(RTex *t)
{
    if (!t) return;
    tex_free_vram(t);
    free(t->block); free((void *)t->units); free((void *)t->parts); free(t->loc); free(t->used); free(t->grid); free(t);
}
void rtex_size(const RTex *t, int *w, int *h) { if (w) *w = t ? t->w : 0; if (h) *h = t ? t->h : 0; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) { t->mr = r; t->mg = g; t->mb = b; } }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) t->alpha = a; }
void rtex_set_blend(RTex *t, RBlend b) { if (t) t->blend = b; }
void rtex_set_scale(RTex *t, RScale s) { (void)t; (void)s; }
void rtex_set_tag(RTex *t, uint32_t tag) { if (t) t->tag = tag; }
void rsat_tex_priority(RTex *t, int reg) { if (t) t->prio = (uint8_t)(reg & 7); }
Ren *rtex_renderer(const RTex *t) { return t ? t->r : &ren; }

/* ---------------------------------------------------------------- the command list */
static Cmd *cmds; static int ncmd;
static int ngouraud;
static uint8_t draw_r, draw_g, draw_b, draw_a = 255; static RBlend draw_blend = R_BLEND_BLEND;
static uint16_t back_color = 0x8000;
static RRect clip, viewport; static bool clip_on, vp_on;
static int scr_w = 320, scr_h = SAT_SCREEN_H;
static bool uclip_active; static RRect uclip_cur;   /* the user clip rectangle set in the list */
static bool pal_drawn;                              /* palette pixels may be in the framebuffer (drawn this frame) */
static int fade_cmd = -1; static uint8_t fade_r, fade_g, fade_b, fade_a;   /* a full-screen translucent fill */
static RTex *backdrop[4]; static int backdrop_x[4], backdrop_y[4], nbackdrops; static bool clear_fb;
static void draw_backdrops(void);

static int32_t fx16(float f);   /* float -> 16.16 without soft-float (below) */
static int16_t pix16(int32_t v) { int i = v >> 16; return (int16_t)(i < -2048 ? -2048 : i > 2047 ? 2047 : i); }   /* floor, clamped */

static Cmd *cmd_new(void)
{
    if (ncmd >= CMD_MAX - 1) return NULL;
    Cmd *c = &cmds[ncmd++];
    uint32_t *w = (uint32_t *)c;   /* 32-byte aligned: eight stores (memset is a call and a loop) */
    w[0] = w[1] = w[2] = w[3] = w[4] = w[5] = w[6] = w[7] = 0;
    return c;
}

static uint16_t rgb555(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)); }

/* the draw's effective clip rectangle in screen pixels (viewport and clip); false when nothing is visible */
static RRect clip_eff; static bool clip_eff_ok, clip_dirty = true;   /* draw_clip's result until the clip changes */

static bool draw_clip(RRect *out)
{
    if (!clip_dirty) { *out = clip_eff; return clip_eff_ok; }
    clip_dirty = false;
    clip_eff_ok = false;
    RRect c = { 0, 0, scr_w, scr_h };
    if (vp_on && !r_rect_intersect(&c, &viewport, &c)) return false;
    if (clip_on) {
        RRect k = clip; if (vp_on) { k.x += viewport.x; k.y += viewport.y; }
        if (!r_rect_intersect(&c, &k, &c)) return false;
    }
    *out = clip_eff = c;
    return clip_eff_ok = true;
}

/* returns the PMOD bits for user clipping (setting the rectangle in the list when it changes) */
static uint16_t clip_bits(const RRect *c, int x0, int y0, int x1, int y1)
{
    if (c->x <= x0 && c->y <= y0 && c->x + c->w > x1 && c->y + c->h > y1) return 0;   /* inside anyway */
    if (!uclip_active || memcmp(&uclip_cur, c, sizeof *c)) {
        Cmd *k = cmd_new(); if (!k) return 0;
        k->ctrl = C_USER_CLIP; k->xa = (int16_t)c->x; k->ya = (int16_t)c->y; k->xc = (int16_t)(c->x + c->w - 1); k->yc = (int16_t)(c->y + c->h - 1);
        uclip_cur = *c; uclip_active = true;
    }
    return PM_CLIP;
}

/* alpha / blend -> colour calculation bits; false: not drawn at all */
static bool blend_bits(uint8_t a, RBlend b, uint16_t *pm)
{
    if (b == R_BLEND_NONE) return true;
    if (b == R_BLEND_ADD) { if (a < 32) return false; *pm |= PM_HALF; return true; }
    if (a < 32) return false;
    if (a < 96) *pm |= PM_HALF | PM_MESH;
    else if (a < 192) *pm |= PM_HALF;
    return true;
}

/* a gouraud table that approximates a colour multiply (r, g, b) / 255; 0 when none is needed */
static uint16_t gour[GOURAUD_MAX];   /* the frame's tables (one colour each, as 4 corners at copy time) */

static uint16_t gouraud_for(uint8_t r, uint8_t g, uint8_t b)
{
    if ((r & g & b) == 255) return 0;
    static uint32_t last_rgb; static int last_i = -1;   /* runs of one colour (a line of text) */
    uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    if (last_i >= 0 && last_i < ngouraud && rgb == last_rgb) return (uint16_t)((GOURAUD_OFF + (uint32_t)last_i * 8) / 8);
    if (ngouraud >= GOURAUD_MAX) return 0;
    int gr = 16 - (255 - r) * 16 / 255, gg = 16 - (255 - g) * 16 / 255, gb = 16 - (255 - b) * 16 / 255;
    uint16_t v = (uint16_t)(0x8000 | gb << 10 | gg << 5 | gr);
    last_rgb = rgb; last_i = ngouraud;
    gour[ngouraud] = v;
    return (uint16_t)((GOURAUD_OFF + (uint32_t)ngouraud++ * 8) / 8);
}

/* the frame's gouraud tables into VDP1 memory, once VDP1 is done with the last frame's (writing them while it draws
 * waits for its bus at every access) */
static void gouraud_upload(void)
{
    volatile uint32_t *d = (volatile uint32_t *)(VDP1_VRAM_BASE + GOURAUD_OFF);
    for (int i = 0; i < ngouraud; i++) { uint32_t v = (uint32_t)gour[i] << 16 | gour[i]; d[i * 2] = v; d[i * 2 + 1] = v; }
}

static int16_t clampc(float v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : (int16_t)floorf(v); }

/* ---------------------------------------------------------------- draw state */
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { (void)r; draw_r = R; draw_g = G; draw_b = B; draw_a = A; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; draw_blend = b; }
void r_set_clip(Ren *r, const RRect *c) { (void)r; clip_on = c != NULL; if (c) clip = *c; clip_dirty = true; }
void r_set_viewport(Ren *r, const RRect *v) { (void)r; vp_on = v != NULL; if (v) viewport = *v; clip_dirty = true; }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { if (out) *out = (RRect){ 0, 0, 0, 0 }; return false; }
    if (out) *out = (RRect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}

void rsat_set_screen(int w, int h) { scr_w = w; scr_h = h; clip_dirty = true; }

/* ---------------------------------------------------------------- primitives */
/* a polygon from four corners in 16.16 viewport coordinates */
static void polygon(const int32_t *xy, uint8_t R, uint8_t G, uint8_t B, uint8_t A, RBlend bl)
{
    RRect c; if (!draw_clip(&c)) return;
    uint16_t pm = PM_ECD | PM_SPD;
    if (!blend_bits(A, bl, &pm)) return;
    if (pal_drawn && (pm & PM_HALF)) pm = (uint16_t)((pm & ~PM_HALF) | PM_MESH);
    int32_t ox = vp_on ? viewport.x << 16 : 0, oy = vp_on ? viewport.y << 16 : 0;
    int16_t v[8];
    int minx = 4096, maxx = -4096, miny = 4096, maxy = -4096;
    for (int i = 0; i < 4; i++) {
        v[i * 2] = pix16(xy[i * 2] + ox); v[i * 2 + 1] = pix16(xy[i * 2 + 1] + oy);
        if (v[i * 2] < minx) minx = v[i * 2];
        if (v[i * 2] > maxx) maxx = v[i * 2];
        if (v[i * 2 + 1] < miny) miny = v[i * 2 + 1];
        if (v[i * 2 + 1] > maxy) maxy = v[i * 2 + 1];
    }
    if (maxx < c.x || maxy < c.y || minx >= c.x + c.w || miny >= c.y + c.h) return;
    pm |= clip_bits(&c, minx, miny, maxx, maxy);
    Cmd *k = cmd_new(); if (!k) return;
    k->ctrl = C_POLYGON; k->pmod = pm; k->colr = rgb555(R, G, B);
    memcpy(&k->xa, v, sizeof v);
    ren.prims++;
}

void r_clear(Ren *r)
{
    (void)r;
    back_color = rgb555(draw_r, draw_g, draw_b);
    ncmd = 3;   /* keep the preamble: everything drawn so far is covered */
    uclip_active = false; pal_drawn = false; fade_cmd = -1;
    draw_backdrops();
}

void r_fill_rect(Ren *r, const RFRect *q)
{
    (void)r;
    int32_t x, y, w, h;
    if (q) { x = fx16(q->x); y = fx16(q->y); w = fx16(q->w); h = fx16(q->h); }
    else { x = y = 0; w = (vp_on ? viewport.w : scr_w) << 16; h = (vp_on ? viewport.h : scr_h) << 16; }
    if (w <= 0 || h <= 0) return;
    int32_t x1 = x + w - (1 << 16), y1 = y + h - (1 << 16);
    int32_t xy[8] = { x, y, x1, y, x1, y1, x, y1 };
    int before = ncmd;
    polygon(xy, draw_r, draw_g, draw_b, draw_a, draw_blend);
    /* a translucent fill of the whole screen: if nothing is drawn after it, rsat_frame_end makes it a colour offset */
    RRect c;
    int vx = vp_on ? viewport.x : 0, vy = vp_on ? viewport.y : 0;
    if (ncmd == before + 1 && draw_blend == R_BLEND_BLEND && draw_a < 255 && draw_clip(&c) && c.x == 0 && c.y == 0 &&
        c.w == scr_w && c.h == scr_h && (x >> 16) + vx <= 0 && (y >> 16) + vy <= 0 &&
        ((x + w) >> 16) + vx >= scr_w && ((y + h) >> 16) + vy >= scr_h) {
        fade_cmd = before; fade_r = draw_r; fade_g = draw_g; fade_b = draw_b; fade_a = draw_a;
    }
}
void r_fill_rects(Ren *r, const RFRect *q, int n) { for (int i = 0; i < n; i++) r_fill_rect(r, &q[i]); }

static void line_cmd(int type, const float *xy, int n)
{
    RRect c; if (!draw_clip(&c)) return;
    uint16_t pm = PM_ECD | PM_SPD;
    if (!blend_bits(draw_a, draw_blend, &pm)) return;
    if (pal_drawn && (pm & PM_HALF)) pm = (uint16_t)((pm & ~PM_HALF) | PM_MESH);
    int32_t ox = vp_on ? viewport.x << 16 : 0, oy = vp_on ? viewport.y << 16 : 0;
    pm |= clip_bits(&c, -4096, -4096, 4096, 4096);
    Cmd *k = cmd_new(); if (!k) return;
    k->ctrl = (uint16_t)type; k->pmod = pm; k->colr = rgb555(draw_r, draw_g, draw_b);
    int16_t *v = &k->xa;
    for (int i = 0; i < 4; i++) { int j = i < n ? i : n - 1; v[i * 2] = pix16(fx16(xy[j * 2]) + ox); v[i * 2 + 1] = pix16(fx16(xy[j * 2 + 1]) + oy); }
    ren.prims++;
}
void r_rect(Ren *r, const RFRect *q)
{
    (void)r;
    if (!q) return;
    float xy[8] = { q->x, q->y, q->x + q->w - 1, q->y, q->x + q->w - 1, q->y + q->h - 1, q->x, q->y + q->h - 1 };
    line_cmd(C_POLYLINE, xy, 4);
}
void r_line(Ren *r, float x0, float y0, float x1, float y1) { (void)r; float xy[4] = { x0, y0, x1, y1 }; line_cmd(C_LINE, xy, 2); }
void r_point(Ren *r, float x, float y) { (void)r; float xy[4] = { x, y, x, y }; line_cmd(C_LINE, xy, 2); }

void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni)
{
    (void)r; (void)t;
    int n = idx ? ni : nv;
    for (int i = 0; i + 2 < n; i += 3) {
        const RVertex *a = &v[idx ? idx[i] : i], *b = &v[idx ? idx[i + 1] : i + 1], *c = &v[idx ? idx[i + 2] : i + 2];
        float cr = (a->color.r + b->color.r + c->color.r) / 3, cg = (a->color.g + b->color.g + c->color.g) / 3;
        float cb = (a->color.b + b->color.b + c->color.b) / 3, ca = (a->color.a + b->color.a + c->color.a) / 3;
        int32_t xy[8] = { fx16(a->position.x), fx16(a->position.y), fx16(b->position.x), fx16(b->position.y),
                          fx16(c->position.x), fx16(c->position.y), fx16(c->position.x), fx16(c->position.y) };
        polygon(xy,
                (uint8_t)(cr * 255), (uint8_t)(cg * 255), (uint8_t)(cb * 255), (uint8_t)(ca * 255), draw_blend);
    }
}

/* ---------------------------------------------------------------- textured draws */
/* the unit whose rectangle is exactly src (binary search: units are sorted by y, then x) */
static const Unit *unit_exact(const RTex *t, int x, int y, int w, int h)
{
    if (t->grid) {
        if (w != t->gw || h != t->gh || x < 0 || y < 0 || x >= t->w || y >= t->h) return NULL;
        int cx = (int)(((uint32_t)x * t->gw_inv) >> 20), cy = (int)(((uint32_t)y * t->gh_inv) >> 20);   /* exact for x, y < 4096 */
        if (cx * t->gw != x || cy * t->gh != y) return NULL;
        int k = t->grid[cy * t->gcols + cx];
        return k ? &t->units[k - 1] : NULL;
    }
    int lo = 0, hi = t->nunits - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2; const Unit *u = &t->units[m];
        if (u->y == y && u->x == x) {
            for (int k = m; k < t->nunits && t->units[k].y == y && t->units[k].x == x; k++) if (t->units[k].w == w && t->units[k].h == h) return &t->units[k];
            for (int k = m - 1; k >= 0 && t->units[k].y == y && t->units[k].x == x; k--) if (t->units[k].w == w && t->units[k].h == h) return &t->units[k];
            return NULL;
        }
        if (u->y < y || (u->y == y && u->x < x)) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

/* float -> 16.16 without the soft-float library (the SH-2 has no FPU): |f| < 32768. The SH-2 has no barrel shifter
 * either (a variable shift is a libgcc loop): the shifts are multiplies by a power of two (mul.l / dmulu.l) */
static const uint32_t POW2[32] = {
    1u, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 11, 1u << 12,
    1u << 13, 1u << 14, 1u << 15, 1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20, 1u << 21, 1u << 22, 1u << 23,
    1u << 24, 1u << 25, 1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, 1u << 31 };

static int32_t fx16(float f)
{
    union { float f; uint32_t u; } v = { f };
    int e = (int)((v.u >> 23) & 255) - 127;
    if (e < -17) return 0;
    uint32_t m = (v.u & 0x7FFFFFu) | 0x800000u;
    int sh = e - 7;   /* the mantissa is 1.23: 16.16 is a shift by e - 23 + 16 */
    int32_t r;
    if (sh >= 0) r = sh > 7 ? 0x7FFFFFFF : (int32_t)(m * POW2[sh]);
    else r = (int32_t)(((uint64_t)m * POW2[32 + sh]) >> 32);   /* m >> -sh: the high word of m * 2^(32 - (-sh)) */
    return (v.u >> 31) ? -r : r;
}
static int fl16(int32_t v) { return v >> 16; }                  /* floor */
static int ce16(int32_t v) { return (v + 0xFFFF) >> 16; }       /* ceiling */

/* A part's command, its screen rectangle known: [ix0, ix1] x [iy0, iy1] inclusive, or quad (4 corners, rotated) */
static void part_emit(RTex *t, int pi, int ix0, int iy0, int ix1, int iy1, RFlip flip, const int16_t *quad, const RRect *c, const RRect *dclip)
{
    const Part *p = &t->parts[pi];
    int fmt = p->fmt & 0x7F;
    uint16_t pm = PM_ECD;
    if (!blend_bits(t->alpha, t->blend, &pm)) return;
    if (fmt == FMT_8BPP && (pm & PM_HALF)) pm = (uint16_t)((pm & ~PM_HALF) | PM_MESH);   /* no VDP1 blending of palette pixels */
    else if (pal_drawn && (pm & PM_HALF)) pm = (uint16_t)((pm & ~PM_HALF) | PM_MESH);
    if (ix1 < c->x || iy1 < c->y || ix0 >= c->x + c->w || iy0 >= c->y + c->h) {
        if (tracing) printf("    part %d culled (%d,%d)-(%d,%d)\n", pi, ix0, iy0, ix1, iy1);
        return;
    }
    uint16_t loc = part_resident(t, pi);
    if (tracing) printf("    part %d fmt %d %dx%d at %d,%d loc %u tint %02X%02X%02X\n", pi, p->fmt, p->wpad, p->h, ix0, iy0, loc, t->mr, t->mg, t->mb);
    if (!loc) return;
    RRect cc = *c;
    if (dclip && !r_rect_intersect(&cc, dclip, &cc)) return;   /* a draw of part of a unit: clip to the destination too */
    int bank = 0;
    if (fmt == FMT_8BPP && (bank = cram_bank(t, t->mr, t->mg, t->mb)) < 0) {
        static int warned; if (!warned++) printf("render: colour RAM full (texture %08X)\n", (unsigned)t->tag);
        return;
    }
    pm |= clip_bits(&cc, ix0, iy0, ix1, iy1);
    uint16_t gr = fmt == FMT_8BPP ? 0 : gouraud_for(t->mr, t->mg, t->mb);
    if (gr) pm |= PM_GOURAUD;
    Cmd *k = cmd_new(); if (!k) return;
    if (fmt == FMT_4BPP) { pm |= PM_LUT; k->colr = loc; k->srca = (uint16_t)(loc + 4); }   /* the table, then the texels */
    else if (fmt == FMT_8BPP) {   /* colour bank: 64 (mode 2), 128 (3) or 256 (4) colours */
        int g = pal_granules(t->npal);
        pm |= (uint16_t)((g == 1 ? 2 : g == 2 ? 3 : 4) << 3); k->colr = (uint16_t)(bank | t->prio << 12); k->srca = loc;
        pal_drawn = true;
    }
    else { pm |= PM_RGB; k->srca = loc; }
    k->pmod = pm; k->grda = gr;
    k->size = (uint16_t)((p->wpad / 8) << 8 | p->h);
    uint16_t fl = (uint16_t)(((flip & R_FLIP_H) ? 0x10 : 0) | ((flip & R_FLIP_V) ? 0x20 : 0));
    if (quad) {   /* rotated: the corners in the part's own order (the map already flipped them) */
        k->ctrl = C_DISTORTED;
        memcpy(&k->xa, quad, 8 * sizeof *quad);
    } else if (ix1 - ix0 + 1 == p->wpad && iy1 - iy0 + 1 == p->h) {
        k->ctrl = C_NORMAL | fl;
        k->xa = (int16_t)ix0; k->ya = (int16_t)iy0;
    } else {
        k->ctrl = C_SCALED | fl;
        k->xa = (int16_t)ix0; k->ya = (int16_t)iy0; k->xc = (int16_t)ix1; k->yc = (int16_t)iy1;
    }
    ren.prims++;
}

/* Unrotated draws (sprites, glyphs, tiles: nearly all of them), in integers: the source rectangle in texels, the
 * destination's corner and scale in 16.16 */
typedef struct { int sx, sy, sw, sh; int32_t dx, dy, kx, ky; RFlip flip; RRect dclip; bool partial; } AMap;

static void draw_part_axis(RTex *t, int pi, const AMap *m, const RRect *c)
{
    const Part *p = &t->parts[pi];
    int u0 = p->x - m->sx, u1 = u0 + p->wpad, v0 = p->y - m->sy, v1 = v0 + p->h;
    if (m->flip & R_FLIP_H) { int a = m->sw - u1; u1 = m->sw - u0; u0 = a; }
    if (m->flip & R_FLIP_V) { int a = m->sh - v1; v1 = m->sh - v0; v0 = a; }
    int32_t x0 = m->dx + (int32_t)((int64_t)u0 * m->kx), x1 = m->dx + (int32_t)((int64_t)u1 * m->kx);
    int32_t y0 = m->dy + (int32_t)((int64_t)v0 * m->ky), y1 = m->dy + (int32_t)((int64_t)v1 * m->ky);
    part_emit(t, pi, fl16(x0), fl16(y0), fl16(x1) - 1, fl16(y1) - 1, m->flip, NULL, c, m->partial ? &m->dclip : NULL);
}

/* Rotated draws: from texture space (the src rect) to the screen: dst, then rotation by angle (degrees, clockwise)
 * about (cx, cy) in screen space, flips. */
typedef struct { float sx, sy, sw, sh, dx, dy, dw, dh; float cs, sn, cx, cy; RFlip flip; } Map;

static void map_pt(const Map *m, float u, float v, float *x, float *y)
{
    float fx = (u - m->sx) / m->sw, fy = (v - m->sy) / m->sh;
    if (m->flip & R_FLIP_H) fx = 1 - fx;
    if (m->flip & R_FLIP_V) fy = 1 - fy;
    float px = m->dx + fx * m->dw, py = m->dy + fy * m->dh;
    float ex = px - m->cx, ey = py - m->cy;
    *x = m->cx + ex * m->cs - ey * m->sn; *y = m->cy + ex * m->sn + ey * m->cs;
}

static void draw_part_rot(RTex *t, int pi, const Map *m, const RRect *c, const RRect *dclip)
{
    const Part *p = &t->parts[pi];
    float ax, ay, bx, by, cxx, cyy, dxx, dyy;
    map_pt(m, p->x, p->y, &ax, &ay); map_pt(m, p->x + p->wpad, p->y, &bx, &by);
    map_pt(m, p->x + p->wpad, p->y + p->h, &cxx, &cyy); map_pt(m, p->x, p->y + p->h, &dxx, &dyy);
    float minx = fminf(fminf(ax, bx), fminf(cxx, dxx)), maxx = fmaxf(fmaxf(ax, bx), fmaxf(cxx, dxx));
    float miny = fminf(fminf(ay, by), fminf(cyy, dyy)), maxy = fmaxf(fmaxf(ay, by), fmaxf(cyy, dyy));
    int16_t q[8] = { clampc(ax), clampc(ay), clampc(bx - 1), clampc(by), clampc(cxx - 1), clampc(cyy - 1), clampc(dxx), clampc(dyy - 1) };
    part_emit(t, pi, (int)floorf(minx), (int)floorf(miny), (int)floorf(maxx) - 1, (int)floorf(maxy) - 1, m->flip, q, c, dclip);
}

static void tex_draw(RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{
    if (!t || !t->nparts) return;
    RRect c; if (!draw_clip(&c)) return;
    int sx = 0, sy = 0, sw = t->w, sh = t->h;
    if (src) { sx = fl16(fx16(src->x)); sy = fl16(fx16(src->y)); sw = fl16(fx16(src->w)); sh = fl16(fx16(src->h)); }
    int32_t dx = 0, dy = 0, dw, dh;
    if (dst) { dx = fx16(dst->x); dy = fx16(dst->y); dw = fx16(dst->w); dh = fx16(dst->h); }
    else { dw = (vp_on ? viewport.w : scr_w) << 16; dh = (vp_on ? viewport.h : scr_h) << 16; }
    if (sw <= 0 || sh <= 0 || dw == 0 || dh == 0) return;
    if (vp_on) { dx += viewport.x << 16; dy += viewport.y << 16; }
    if (dw < 0) { dw = -dw; flip ^= R_FLIP_H; }   /* r_tex_batch's mirrored tiles */
    const Unit *u = unit_exact(t, sx, sy, sw, sh);
    bool whole = u && u->x == sx && u->y == sy;
    if (tracing) printf("  tex %08X src %d,%d %dx%d dst %d,%d %dx%d a%u %s ncmd %d\n", (unsigned)t->tag, sx, sy, sw, sh,
                        fl16(dx), fl16(dy), fl16(dw), fl16(dh), t->alpha, u ? "unit" : "rect", ncmd);
    RRect dclip = { fl16(dx), fl16(dy), ce16(dw), ce16(dh) };
    uint64_t abits; memcpy(&abits, &angle, sizeof abits);   /* angle == 0 without a soft-double compare */
    if (!(abits << 1)) {
        AMap m = { sx, sy, sw, sh, dx, dy, dw == sw << 16 ? 1 << 16 : (int32_t)(((int64_t)dw) / sw),
                   dh == sh << 16 ? 1 << 16 : (int32_t)(((int64_t)dh) / sh), flip, dclip, !whole };
        if (whole) { for (int i = 0; i < u->n; i++) draw_part_axis(t, u->first + i, &m, &c); return; }
        for (int i = 0; i < t->nparts; i++) {   /* any other rectangle: every part that overlaps it, clipped to the destination */
            const Part *p = &t->parts[i];
            if (p->x + p->w <= sx || p->y + p->h <= sy || p->x >= sx + sw || p->y >= sy + sh) continue;
            draw_part_axis(t, i, &m, &c);
        }
        return;
    }
    float a = (float)angle * 3.14159265f / 180.0f;
    float fdx = dx / 65536.0f, fdy = dy / 65536.0f, fdw = dw / 65536.0f, fdh = dh / 65536.0f;
    Map m = { (float)sx, (float)sy, (float)sw, (float)sh, fdx, fdy, fdw, fdh, cosf(a), sinf(a),
              fdx + (center ? center->x : fdw * 0.5f), fdy + (center ? center->y : fdh * 0.5f), flip };
    if (whole) { for (int i = 0; i < u->n; i++) draw_part_rot(t, u->first + i, &m, &c, NULL); return; }
    for (int i = 0; i < t->nparts; i++) {
        const Part *p = &t->parts[i];
        if (p->x + p->w <= sx || p->y + p->h <= sy || p->x >= sx + sw || p->y >= sy + sh) continue;
        draw_part_rot(t, i, &m, &c, &dclip);
    }
}

static void tex_draw_timed(RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{
    uint32_t a = sat_timer_us();
    tex_draw(t, src, dst, angle, center, flip);
    tm_tex += sat_timer_us() - a; tm_ntex++;
}
#define tex_draw tex_draw_timed
void r_tex(Ren *r, RTex *t, const RFRect *src, const RFRect *dst) { (void)r; tex_draw(t, src, dst, 0, NULL, R_FLIP_NONE); }
void r_tex_rot(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{ (void)r; tex_draw(t, src, dst, angle, center, flip); }
void r_tex_batch(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, int n) { (void)r; for (int i = 0; i < n; i++) tex_draw(t, &src[i], &dst[i], 0, NULL, R_FLIP_NONE); }

/* ---------------------------------------------------------------- the floor (Mode 7): VDP2 RBG0 comes with M9 */
RFloor *r_floor_create(Ren *r, const RFloorDesc *d) { (void)r; (void)d; return calloc(1, sizeof(RFloor)); }
void    r_floor_cells_changed(RFloor *f) { (void)f; }
void    r_floor_draw(Ren *r, RFloor *f, const RFloorView *v) { (void)r; (void)f; (void)v; }
void    r_floor_destroy(RFloor *f) { free(f); }

/* ---------------------------------------------------------------- frames */
void rsat_init(void)
{
    const vdp1_env_t env = {
        .bpp = VDP1_ENV_BPP_16, .rotation = VDP1_ENV_ROTATION_0, .color_mode = VDP1_ENV_COLOR_MODE_RGB_PALETTE,
        .sprite_type = 5, .erase_color = RGB1555(0, 0, 0, 0),
        .erase_points = { { 0, 0 }, { SAT_WIDE_W - 1, SAT_SCREEN_H - 1 } } };
    vdp1_env_set(&env);
    vdp2_sprite_priority_set(0, 6);
    vdp2_cram_mode_set(1);
    const char *tr = plat_getenv("SABER_RTRACE");
    trace_from = tr ? atoi(tr) : 0;
    for (int i = 0; i < CRAM_GRAN; i++) cowner[i] = -1;
    cmds = hw_memalign(32, sizeof(Cmd) * CMD_MAX);
    staging = hw_memalign(32, STAGING_SIZE);
    if (!cmds || !staging) printf("render: no RAM for the command list\n");
    vdp1_sync_interval_set(-1);   /* variable: the framebuffers change once VDP1 has finished the frame (AUTO (0) changes
                                   * them every field and cuts off a frame VDP1 needs longer for) */
}

void rsat_frame_begin(void)
{
    frame_no++;
    tracing = trace_from > 0 && traced < 6 && frame_no >= (unsigned)trace_from && (frame_no - (unsigned)trace_from) % 10 == 0;
    if (tracing) { traced++; printf("render trace: frame %u\n", (unsigned)frame_no); }
    ren.prims = 0; ngouraud = 0; uploads_frame = upload_bytes_frame = 0;
    ncmd = 0; uclip_active = false; pal_drawn = false; fade_cmd = -1;
    Cmd *k = cmd_new(); k->ctrl = C_SYS_CLIP; k->xc = (int16_t)(scr_w - 1); k->yc = (int16_t)(scr_h - 1);
    k = cmd_new(); k->ctrl = C_USER_CLIP; k->xc = (int16_t)(scr_w - 1); k->yc = (int16_t)(scr_h - 1);
    k = cmd_new(); k->ctrl = C_LOCAL;
    draw_backdrops();
}

void rsat_set_backdrops(RTex **t, const int *x, const int *y, int n, bool clear)
{
    clear_fb = clear;
    nbackdrops = n < 4 ? n : 4;
    for (int i = 0; i < nbackdrops; i++) { backdrop[i] = t[i]; backdrop_x[i] = x[i]; backdrop_y[i] = y[i]; }
}

/* the VDP2 planes' backdrops (vdp2_planes.c): palette sprites under the planes, first in the list so every other
 * sprite covers them. With the planes on, the framebuffer is cleared to transparent first: the vblank erase of the
 * variable frame change doesn't get through a whole 16bpp screen (the bottom ~40 lines keep the last frame's sprites) */
static void draw_backdrops(void)
{
    if (clear_fb) {
        Cmd *k = cmd_new();
        if (k) {
            k->ctrl = C_POLYGON; k->pmod = PM_ECD | PM_SPD; k->colr = 0;
            k->xa = 0; k->ya = 0; k->xb = (int16_t)(scr_w - 1); k->yb = 0;
            k->xc = (int16_t)(scr_w - 1); k->yc = (int16_t)(scr_h - 1); k->xd = 0; k->yd = (int16_t)(scr_h - 1);
        }
    }
    bool vp = vp_on, cl = clip_on;
    vp_on = clip_on = false; clip_dirty = true;
    for (int i = 0; i < nbackdrops; i++) {
        RFRect d = { (float)backdrop_x[i], (float)backdrop_y[i], (float)backdrop[i]->w, (float)backdrop[i]->h };
        tex_draw(backdrop[i], NULL, &d, 0, NULL, R_FLIP_NONE);
    }
    vp_on = vp; clip_on = cl; clip_dirty = true;
    pal_drawn = false;   /* under the planes: nothing is blended over them */
}

/* the colour offset (VDP2, every layer): the fade fill ending the frame, lerp towards its colour approximated as an add */
static void colour_offset(void)
{
    vdp2_ioregs_t *regs = vdp2_regs_get();
    if (fade_cmd < 0 || fade_cmd != ncmd - 1) { regs->clofen = 0; return; }
    ncmd--;   /* the fill's polygon: not drawn */
    int a = fade_a;
    int orr = a * (fade_r * 2 - 255) / 255, og = a * (fade_g * 2 - 255) / 255, ob = a * (fade_b * 2 - 255) / 255;
    regs->clofen = 0x7F;   /* NBG0-3, RBG0, back screen, sprites */
    regs->clofsl = 0;      /* offset A */
    regs->coar = (uint16_t)(orr & 0x1FF); regs->coag = (uint16_t)(og & 0x1FF); regs->coab = (uint16_t)(ob & 0x1FF);
}

/* SABER_PERF: where the end of a frame goes (microseconds, summed; rsat_timing) */

void rsat_timing(uint32_t *planes, uint32_t *vdp1_wait, uint32_t *put, uint32_t *frames)
{
    *planes = tm_planes; *vdp1_wait = tm_wait; *put = tm_put; *frames = tm_frames;
    if (tm_frames) printf("[perf] textured draws %u us a frame (%u calls, uploads %u us)\n",
                          (unsigned)(tm_tex / tm_frames), (unsigned)(tm_ntex / tm_frames), (unsigned)(tm_upl / tm_frames));
    tm_planes = tm_wait = tm_put = tm_frames = tm_tex = tm_ntex = tm_upl = 0;
}

void rsat_frame_end(void)
{
    uint32_t t0 = sat_timer_us();
    sat_planes_frame(scr_w);
    uint32_t t1 = sat_timer_us();
    colour_offset();
    Cmd *k = &cmds[ncmd++]; memset(k, 0, sizeof *k); k->ctrl = C_END;
    vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), (rgb1555_t){ .raw = back_color });
    vdp1_sync_wait();
    gouraud_upload();
    uint32_t t2 = sat_timer_us();
    vdp1_sync_cmdt_put((const vdp1_cmdt_t *)cmds, (uint16_t)ncmd, 0);
    vdp1_sync_render();
    vdp1_sync();
    uint32_t t3 = sat_timer_us();
    tm_planes += t1 - t0; tm_wait += t2 - t1; tm_put += t3 - t2; tm_frames++;
}

void rsat_stats(unsigned *parts_resident, unsigned *vram_used, unsigned *uploads, unsigned *evicted)
{
    uint32_t used = 0; for (int i = 0; i < nslots; i++) used += slots[i].size;
    *parts_resident = (unsigned)nslots; *vram_used = used; *uploads = uploads_frame; *evicted = evictions;
}

/* SABER_RBENCH=1 (debug): the cost of one textured draw with a warm cache (1000 draws of one 32x32 sprite) */
void rsat_bench(void)
{
    uint32_t px[32 * 32];
    for (int i = 0; i < 32 * 32; i++) px[i] = 0xFF00FF00u | (uint32_t)(i & 255);
    RTex *t = rtex_create(&ren, 32, 32, R_TEX_STATIC, px);
    if (!t) return;
    rsat_frame_begin();
    RFRect src = { 0, 0, 32, 32 }, dst = { 10, 10, 32, 32 };
    uint32_t a = sat_timer_us();
    for (int i = 0; i < 1000; i++) { ncmd = 3; r_tex(&ren, t, &src, &dst); }
    uint32_t b = sat_timer_us();
    for (int i = 0; i < 1000; i++) { (void)sat_timer_us(); }
    uint32_t c = sat_timer_us();
    printf("[bench] r_tex: %u ns a draw (warm), sat_timer_us: %u ns\n", (unsigned)(b - a), (unsigned)(c - b));
    ncmd = 3;
    rtex_destroy(t);
}

/* SABER_SHOT: nothing to save on the console (the harness takes screenshots from the emulator) */
