/* platform/render.h on the Saturn: every draw is a VDP1 command (plan 4.4).
 *
 * Textures are "SAT1" blocks (tools/saturn/satbake.py): the rectangles the game draws out of a texture ("units": a
 * sprite frame, a tile, a glyph) are stored as VDP1-ready "parts" (4bpp with a colour lookup table, or 16bpp RGB),
 * LZ4-compressed. A texture keeps its block in work RAM (low RAM for big ones); a part is decoded into video memory the
 * first time it is drawn and stays there until the space is needed (least recently used first; never a part drawn in
 * this frame or the one VDP1 may still be drawing). Runtime RGBA textures (rtex_create) are turned into such a block.
 *
 * The frame's commands are built in high work RAM and DMA'd to VDP1 at rsat_frame_end. Pixels are RGB (the colour
 * tables hold RGB555 with bit 15 set), so no CRAM is used yet. VDP1 has no general alpha or colour multiply:
 *   alpha:  < 32 not drawn, < 96 mesh + half-transparency (~25 %), < 192 half-transparency, else opaque;
 *   add:    half-transparency (VDP2 colour calculation comes with the palette sprites, plan 4.4);
 *   colour mod: gouraud shading, which adds or subtracts per channel (an approximation of the multiply). */
#include "../render.h"
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
    uint8_t mr, mg, mb, alpha; RBlend blend; uint32_t tag;
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
typedef struct { uint32_t off, size; RTex *t; uint16_t part; uint32_t used; } Slot;   /* sorted by off */
#define MAX_SLOTS 1536
static Slot slots[MAX_SLOTS]; static int nslots;
static uint32_t frame_no = 2;
static uint8_t *staging;
static unsigned uploads_frame, upload_bytes_frame, evictions;

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
            if (slots[i].used + 1 < frame_no && (victim < 0 || slots[i].used < slots[victim].used)) victim = i;
        if (victim < 0) return false;
        slot_remove(victim);
        evictions++;
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
    return (p->fmt & 0x7F) == 0 ? 32u + (uint32_t)p->wpad * p->h / 2 : (uint32_t)p->wpad * p->h * 2;
}

/* the part's address in video memory / 8 (uploading it if needed), 0 if there is no room */
static uint16_t part_resident(RTex *t, int i)
{
    const Part *p = &t->parts[i];
    if (t->loc[i]) {
        int s = slot_find((uint32_t)t->loc[i] * 8);
        if (s >= 0) slots[s].used = frame_no;
        return t->loc[i];
    }
    uint32_t raw = part_raw_size(p), off;
    if (raw > STAGING_SIZE || !vram_alloc(raw, t, i, &off)) {
        static int warned; if (!warned++) printf("render: no video memory for a %u byte part (%d parts resident)\n", (unsigned)raw, nslots);
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
    uploads_frame++; upload_bytes_frame += raw;
    t->loc[i] = (uint16_t)(off / 8);
    return t->loc[i];
}

static void tex_free_vram(RTex *t)
{
    for (int i = 0; i < t->nparts; i++)
        if (t->loc[i]) { int s = slot_find((uint32_t)t->loc[i] * 8); if (s >= 0) { slots[s].t = NULL; slot_remove(s); } t->loc[i] = 0; }
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

static RTex *tex_from_block(Ren *r, uint8_t *block, size_t size, bool owned)
{
    if (size < 32 || memcmp(block, "SAT1", 4)) return NULL;
    RTex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->r = r; t->w = le16(block + 4); t->h = le16(block + 6);
    t->nunits = le16(block + 8); t->nparts = le16(block + 10);
    uint32_t uoff = le32(block + 24), poff = le32(block + 28);
    t->block = block; t->size = size; t->owned_block = owned;
    /* the tables into host structs (the block's are big-endian and packed) */
    Unit *u = malloc(sizeof(Unit) * (size_t)(t->nunits ? t->nunits : 1));
    Part *p = malloc(sizeof(Part) * (size_t)(t->nparts ? t->nparts : 1));
    t->loc = calloc((size_t)(t->nparts ? t->nparts : 1), sizeof *t->loc);
    if (!u || !p || !t->loc || uoff + 12u * t->nunits > size || poff + 20u * t->nparts > size) {
        free(u); free(p); free(t->loc); free(t); return NULL;
    }
    for (int i = 0; i < t->nunits; i++) {
        const uint8_t *d = block + uoff + 12 * i;
        u[i] = (Unit){ be16(d), be16(d + 2), be16(d + 4), be16(d + 6), be16(d + 8), be16(d + 10) };
    }
    for (int i = 0; i < t->nparts; i++) {
        const uint8_t *d = block + poff + 20 * i;
        p[i] = (Part){ be16(d), be16(d + 2), be16(d + 4), be16(d + 6), be16(d + 8), d[10], 0, be32(d + 12), be32(d + 16) };
    }
    t->units = u; t->parts = p;
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
    free(t->block); free((void *)t->units); free((void *)t->parts); free(t->loc);
    t->block = n->block; t->size = n->size; t->units = n->units; t->parts = n->parts; t->loc = n->loc;
    t->nunits = n->nunits; t->nparts = n->nparts;
    free(n);
}

void rtex_destroy(RTex *t)
{
    if (!t) return;
    tex_free_vram(t);
    free(t->block); free((void *)t->units); free((void *)t->parts); free(t->loc); free(t);
}
void rtex_size(const RTex *t, int *w, int *h) { if (w) *w = t ? t->w : 0; if (h) *h = t ? t->h : 0; }
void rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b) { if (t) { t->mr = r; t->mg = g; t->mb = b; } }
void rtex_set_alpha_mod(RTex *t, uint8_t a) { if (t) t->alpha = a; }
void rtex_set_blend(RTex *t, RBlend b) { if (t) t->blend = b; }
void rtex_set_scale(RTex *t, RScale s) { (void)t; (void)s; }
void rtex_set_tag(RTex *t, uint32_t tag) { if (t) t->tag = tag; }
Ren *rtex_renderer(const RTex *t) { return t ? t->r : &ren; }

/* ---------------------------------------------------------------- the command list */
static Cmd *cmds; static int ncmd;
static int ngouraud;
static uint8_t draw_r, draw_g, draw_b, draw_a = 255; static RBlend draw_blend = R_BLEND_BLEND;
static uint16_t back_color = 0x8000;
static RRect clip, viewport; static bool clip_on, vp_on;
static int scr_w = 320, scr_h = SAT_SCREEN_H;
static bool uclip_active; static RRect uclip_cur;   /* the user clip rectangle set in the list */

static Cmd *cmd_new(void)
{
    if (ncmd >= CMD_MAX - 1) return NULL;
    Cmd *c = &cmds[ncmd++];
    memset(c, 0, sizeof *c);
    return c;
}

static uint16_t rgb555(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)); }

/* the draw's effective clip rectangle in screen pixels (viewport and clip); false when nothing is visible */
static bool draw_clip(RRect *out)
{
    RRect c = { 0, 0, scr_w, scr_h };
    if (vp_on && !r_rect_intersect(&c, &viewport, &c)) return false;
    if (clip_on) {
        RRect k = clip; if (vp_on) { k.x += viewport.x; k.y += viewport.y; }
        if (!r_rect_intersect(&c, &k, &c)) return false;
    }
    *out = c;
    return true;
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
static uint16_t gouraud_for(uint8_t r, uint8_t g, uint8_t b)
{
    if ((r & g & b) == 255 || ngouraud >= GOURAUD_MAX) return 0;
    int gr = 16 - (255 - r) * 16 / 255, gg = 16 - (255 - g) * 16 / 255, gb = 16 - (255 - b) * 16 / 255;
    uint16_t v = (uint16_t)(0x8000 | gb << 10 | gg << 5 | gr);
    uint32_t off = GOURAUD_OFF + (uint32_t)ngouraud++ * 8;
    volatile uint16_t *d = (volatile uint16_t *)(VDP1_VRAM_BASE + off);
    d[0] = d[1] = d[2] = d[3] = v;
    return (uint16_t)(off / 8);
}

static int16_t clampc(float v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : (int16_t)floorf(v); }

/* ---------------------------------------------------------------- draw state */
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A) { (void)r; draw_r = R; draw_g = G; draw_b = B; draw_a = A; }
void r_set_draw_blend(Ren *r, RBlend b) { (void)r; draw_blend = b; }
void r_set_clip(Ren *r, const RRect *c) { (void)r; clip_on = c != NULL; if (c) clip = *c; }
void r_set_viewport(Ren *r, const RRect *v) { (void)r; vp_on = v != NULL; if (v) viewport = *v; }
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out)
{
    int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { if (out) *out = (RRect){ 0, 0, 0, 0 }; return false; }
    if (out) *out = (RRect){ x0, y0, x1 - x0, y1 - y0 };
    return true;
}

void rsat_set_screen(int w, int h) { scr_w = w; scr_h = h; }

/* ---------------------------------------------------------------- primitives */
static void polygon(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t R, uint8_t G, uint8_t B, uint8_t A, RBlend bl)
{
    RRect c; if (!draw_clip(&c)) return;
    uint16_t pm = PM_ECD | PM_SPD;
    if (!blend_bits(A, bl, &pm)) return;
    float ox = vp_on ? viewport.x : 0, oy = vp_on ? viewport.y : 0;
    float minx = fminf(fminf(x0, x1), fminf(x2, x3)) + ox, maxx = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3)) + ox;
    float miny = fminf(fminf(y0, y1), fminf(y2, y3)) + oy, maxy = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3)) + oy;
    if (maxx < c.x || maxy < c.y || minx >= c.x + c.w || miny >= c.y + c.h) return;
    pm |= clip_bits(&c, (int)minx, (int)miny, (int)maxx, (int)maxy);
    Cmd *k = cmd_new(); if (!k) return;
    k->ctrl = C_POLYGON; k->pmod = pm; k->colr = rgb555(R, G, B);
    k->xa = clampc(x0 + ox); k->ya = clampc(y0 + oy); k->xb = clampc(x1 + ox); k->yb = clampc(y1 + oy);
    k->xc = clampc(x2 + ox); k->yc = clampc(y2 + oy); k->xd = clampc(x3 + ox); k->yd = clampc(y3 + oy);
    ren.prims++;
}

void r_clear(Ren *r)
{
    (void)r;
    back_color = rgb555(draw_r, draw_g, draw_b);
    ncmd = 3;   /* keep the preamble: everything drawn so far is covered */
    uclip_active = false;
}

void r_fill_rect(Ren *r, const RFRect *q)
{
    (void)r;
    RFRect f = q ? *q : (RFRect){ 0, 0, vp_on ? (float)viewport.w : (float)scr_w, vp_on ? (float)viewport.h : (float)scr_h };
    if (f.w <= 0 || f.h <= 0) return;
    float x1 = f.x + f.w - 1, y1 = f.y + f.h - 1;
    polygon(f.x, f.y, x1, f.y, x1, y1, f.x, y1, draw_r, draw_g, draw_b, draw_a, draw_blend);
}
void r_fill_rects(Ren *r, const RFRect *q, int n) { for (int i = 0; i < n; i++) r_fill_rect(r, &q[i]); }

static void line_cmd(int type, const float *xy, int n)
{
    RRect c; if (!draw_clip(&c)) return;
    uint16_t pm = PM_ECD | PM_SPD;
    if (!blend_bits(draw_a, draw_blend, &pm)) return;
    float ox = vp_on ? viewport.x : 0, oy = vp_on ? viewport.y : 0;
    pm |= clip_bits(&c, -4096, -4096, 4096, 4096);
    Cmd *k = cmd_new(); if (!k) return;
    k->ctrl = (uint16_t)type; k->pmod = pm; k->colr = rgb555(draw_r, draw_g, draw_b);
    int16_t *v = &k->xa;
    for (int i = 0; i < 4; i++) { int j = i < n ? i : n - 1; v[i * 2] = clampc(xy[j * 2] + ox); v[i * 2 + 1] = clampc(xy[j * 2 + 1] + oy); }
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
        polygon(a->position.x, a->position.y, b->position.x, b->position.y, c->position.x, c->position.y, c->position.x, c->position.y,
                (uint8_t)(cr * 255), (uint8_t)(cg * 255), (uint8_t)(cb * 255), (uint8_t)(ca * 255), draw_blend);
    }
}

/* ---------------------------------------------------------------- textured draws */
/* the unit whose rectangle is exactly src (binary search: units are sorted by y, then x) */
static const Unit *unit_exact(const RTex *t, int x, int y, int w, int h)
{
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

/* One part, mapped from texture space (the src rect) to the screen: dst, then rotation by angle (degrees, clockwise)
 * about (cx, cy) in screen space, flips. */
typedef struct { float sx, sy, sw, sh, dx, dy, dw, dh; float cs, sn, cx, cy; bool rot; RFlip flip; } Map;

static void map_pt(const Map *m, float u, float v, float *x, float *y)
{
    float fx = (u - m->sx) / m->sw, fy = (v - m->sy) / m->sh;
    if (m->flip & R_FLIP_H) fx = 1 - fx;
    if (m->flip & R_FLIP_V) fy = 1 - fy;
    float px = m->dx + fx * m->dw, py = m->dy + fy * m->dh;
    if (m->rot) { float ex = px - m->cx, ey = py - m->cy; px = m->cx + ex * m->cs - ey * m->sn; py = m->cy + ex * m->sn + ey * m->cs; }
    *x = px; *y = py;
}

static void draw_part(RTex *t, int pi, const Map *m, const RRect *c, bool partial)
{
    const Part *p = &t->parts[pi];
    uint16_t pm = PM_ECD;
    if (!blend_bits(t->alpha, t->blend, &pm)) return;
    /* the part's corners on the screen (its padded width: the padding is transparent) */
    float ax, ay, bx, by, cxx, cyy, dxx, dyy;
    map_pt(m, p->x, p->y, &ax, &ay); map_pt(m, p->x + p->wpad, p->y, &bx, &by);
    map_pt(m, p->x + p->wpad, p->y + p->h, &cxx, &cyy); map_pt(m, p->x, p->y + p->h, &dxx, &dyy);
    float minx = fminf(fminf(ax, bx), fminf(cxx, dxx)), maxx = fmaxf(fmaxf(ax, bx), fmaxf(cxx, dxx));
    float miny = fminf(fminf(ay, by), fminf(cyy, dyy)), maxy = fmaxf(fmaxf(ay, by), fmaxf(cyy, dyy));
    if (maxx <= c->x || maxy <= c->y || minx >= c->x + c->w || miny >= c->y + c->h) return;
    uint16_t loc = part_resident(t, pi);
    if (!loc) return;
    RRect cc = *c;
    if (partial) {   /* a draw of part of a unit: clip to the destination rectangle too */
        RRect d = { (int)floorf(m->dx), (int)floorf(m->dy), (int)ceilf(m->dw), (int)ceilf(m->dh) };
        if (!r_rect_intersect(&cc, &d, &cc)) return;
    }
    pm |= clip_bits(&cc, (int)minx, (int)miny, (int)maxx - 1, (int)maxy - 1);
    uint16_t gr = gouraud_for(t->mr, t->mg, t->mb);
    if (gr) pm |= PM_GOURAUD;
    Cmd *k = cmd_new(); if (!k) return;
    if ((p->fmt & 0x7F) == 0) { pm |= PM_LUT; k->colr = loc; k->srca = (uint16_t)(loc + 4); }   /* the table, then the texels */
    else { pm |= PM_RGB; k->srca = loc; }
    k->pmod = pm; k->grda = gr;
    k->size = (uint16_t)((p->wpad / 8) << 8 | p->h);
    bool axis = !m->rot;
    int ix0 = (int)floorf(minx), iy0 = (int)floorf(miny), ix1 = (int)floorf(maxx) - 1, iy1 = (int)floorf(maxy) - 1;
    if (axis && ix1 - ix0 + 1 == p->wpad && iy1 - iy0 + 1 == p->h) {
        k->ctrl = C_NORMAL | ((m->flip & R_FLIP_H) ? 0x10 : 0) | ((m->flip & R_FLIP_V) ? 0x20 : 0);
        k->xa = (int16_t)ix0; k->ya = (int16_t)iy0;
    } else if (axis) {
        k->ctrl = C_SCALED | ((m->flip & R_FLIP_H) ? 0x10 : 0) | ((m->flip & R_FLIP_V) ? 0x20 : 0);
        k->xa = (int16_t)ix0; k->ya = (int16_t)iy0; k->xc = (int16_t)ix1; k->yc = (int16_t)iy1;
    } else {   /* rotated: the corners in the part's own order (the map already flipped them) */
        k->ctrl = C_DISTORTED;
        k->xa = clampc(ax); k->ya = clampc(ay); k->xb = clampc(bx - 1); k->yb = clampc(by);
        k->xc = clampc(cxx - 1); k->yc = clampc(cyy - 1); k->xd = clampc(dxx); k->yd = clampc(dyy - 1);
    }
    ren.prims++;
}

static void tex_draw(RTex *t, const RFRect *src, const RFRect *dst, double angle, const RFPoint *center, RFlip flip)
{
    if (!t || !t->nparts) return;
    RRect c; if (!draw_clip(&c)) return;
    RFRect s = src ? *src : (RFRect){ 0, 0, (float)t->w, (float)t->h };
    RFRect d = dst ? *dst : (RFRect){ 0, 0, vp_on ? (float)viewport.w : (float)scr_w, vp_on ? (float)viewport.h : (float)scr_h };
    if (s.w <= 0 || s.h <= 0 || d.w == 0 || d.h == 0) return;
    if (vp_on) { d.x += viewport.x; d.y += viewport.y; }
    Map m = { s.x, s.y, s.w, s.h, d.x, d.y, d.w, d.h, 1, 0, 0, 0, false, flip };
    if (d.w < 0) { m.dx = d.x; m.dw = -d.w; m.flip ^= R_FLIP_H; }   /* r_tex_batch's mirrored tiles */
    if (angle != 0) {
        float a = (float)angle * 3.14159265f / 180.0f;
        m.cs = cosf(a); m.sn = sinf(a); m.rot = true;
        m.cx = m.dx + (center ? center->x : m.dw * 0.5f); m.cy = m.dy + (center ? center->y : m.dh * 0.5f);
    }
    const Unit *u = unit_exact(t, (int)s.x, (int)s.y, (int)s.w, (int)s.h);
    if (u && u->x == s.x && u->y == s.y) {
        for (int i = 0; i < u->n; i++) draw_part(t, u->first + i, &m, &c, false);
        return;
    }
    /* any other rectangle: every part that overlaps it, clipped to the destination */
    for (int i = 0; i < t->nparts; i++) {
        const Part *p = &t->parts[i];
        if (p->x + p->w <= s.x || p->y + p->h <= s.y || p->x >= s.x + s.w || p->y >= s.y + s.h) continue;
        draw_part(t, i, &m, &c, true);
    }
}

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
    cmds = hw_memalign(32, sizeof(Cmd) * CMD_MAX);
    staging = hw_memalign(32, STAGING_SIZE);
    if (!cmds || !staging) printf("render: no RAM for the command list\n");
    vdp1_sync_interval_set(0);
}

void rsat_frame_begin(void)
{
    frame_no++;
    ren.prims = 0; ngouraud = 0; uploads_frame = upload_bytes_frame = 0;
    ncmd = 0; uclip_active = false;
    Cmd *k = cmd_new(); k->ctrl = C_SYS_CLIP; k->xc = (int16_t)(scr_w - 1); k->yc = (int16_t)(scr_h - 1);
    k = cmd_new(); k->ctrl = C_USER_CLIP; k->xc = (int16_t)(scr_w - 1); k->yc = (int16_t)(scr_h - 1);
    k = cmd_new(); k->ctrl = C_LOCAL;
}

void rsat_frame_end(void)
{
    Cmd *k = &cmds[ncmd++]; memset(k, 0, sizeof *k); k->ctrl = C_END;
    vdp2_scrn_back_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), (rgb1555_t){ .raw = back_color });
    vdp1_sync_wait();
    vdp1_sync_cmdt_put((const vdp1_cmdt_t *)cmds, (uint16_t)ncmd, 0);
    vdp1_sync_render();
    vdp1_sync();
}

void rsat_stats(unsigned *parts_resident, unsigned *vram_used, unsigned *uploads, unsigned *evicted)
{
    uint32_t used = 0; for (int i = 0; i < nslots; i++) used += slots[i].size;
    *parts_resident = (unsigned)nslots; *vram_used = used; *uploads = uploads_frame; *evicted = evictions;
}

/* SABER_SHOT: nothing to save on the console (the harness takes screenshots from the emulator) */
