#include "gfx.h"
#include "pack.h"
#include "lzo1z.h"
#include "platform/plat.h"
#include "assets.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TILES_PER_ROW 32
static Ren *R;
static uint32_t g_frame = 2;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static inline uint32_t argb1555_to_rgba(uint16_t v)
{
    uint32_t a = (v & 0x8000) ? 255 : 0;
    uint32_t rr = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    rr = (rr * 255 + 15) / 31; g = (g * 255 + 15) / 31; b = (b * 255 + 15) / 31;
    return a << 24 | b << 16 | g << 8 | rr;   /* RGBA bytes in memory */
}
static inline uint32_t argb4444_to_rgba(uint16_t v)
{
    uint32_t a = (v >> 12) * 17, rr = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17;
    return a << 24 | b << 16 | g << 8 | rr;
}

static RTex *finish_tex(RTex *t, uint32_t id, int w, int h)
{
    if (!t) { fprintf(stderr, "tex %08X: %dx%d failed\n", id, w, h); return NULL; }
    rtex_set_blend(t, R_BLEND_BLEND);
    rtex_set_scale(t, R_SCALE_NEAREST);
    rtex_set_tag(t, id);
    return t;
}
static RTex *make_tex(uint32_t id, int w, int h, const uint32_t *px) { return finish_tex(rtex_create(R, w, h, R_TEX_STATIC, px), id, w, h); }

#ifdef PLAT_BAKED_ASSETS
/* A texture baked for the console ahead of time (a RES_TEX block of tex.pck: tools/dc/texbake.py's "PVT1", tools/saturn/
 * satbake.py's "SAT1", the same header up to the meta fields): pack sprites, cblocks
 * and fonts under their own id, our images under asset_key(path). One read of a block that goes to video memory as it
 * is. meta (if asked) gets a copy of the layout data baked with it (sprite frames, a cblock's cell grid). */
static RTex *baked_tex(uint32_t key, int *w, int *h, uint8_t **meta)
{
    const PackEntry *e = packs_find_type(key, RES_TEX);
    if (!e || e->size < 32 || (memcmp(e->data, "PVT1", 4) && memcmp(e->data, "SAT1", 4))) return NULL;   /* Dreamcast / Saturn */
    const uint8_t *d = e->data;
    *w = rd16(d + 4); *h = rd16(d + 6);
    if (meta) {
        uint32_t off = rd32(d + 16), n = rd32(d + 20);
        *meta = malloc(n + 2);
        if (*meta) memcpy(*meta, d + off, n);
    }
    RTex *t = rtex_create_baked(R, (uint8_t *)d, e->size);   /* may remap the block's palette indices in place */
    packs_release_type(key, RES_TEX);
    return finish_tex(t, key, *w, *h);
}
#endif

RTex *gfx_image_tex(const char *path, int *w, int *h)
{
    int ww = 0, hh = 0; RTex *t = NULL;
    if (!path) return NULL;
#ifdef PLAT_BAKED_ASSETS
    t = baked_tex(asset_key(path), &ww, &hh, NULL);
#endif
    if (!t) {
        uint32_t *px = png_load_rgba(path, &ww, &hh);
        if (!px) return NULL;
        t = make_tex(asset_key(path), ww, hh, px);
        free(px);
    }
    if (w) *w = ww;
    if (h) *h = hh;
    return t;
}

/* ---- cblock cache ---- */
#define MAX_CB 64
static CBlock g_cb[MAX_CB]; static int g_ncb;

/* the tile sheet of a cblock blob: TILES_PER_ROW tiles per row, row by row from the ARGB1555 tiles */
typedef struct { const CBlock *c; const uint8_t *raw; } SheetSrc;
static void sheet_rows(void *ud, int y0, int n, uint32_t *out)
{
    const SheetSrc *s = ud; const CBlock *c = s->c;
    int W = TILES_PER_ROW * c->tw;
    for (int y = y0; y < y0 + n; y++, out += W) {
        int trow = y / c->th, py = y % c->th;
        for (int k = 0; k < TILES_PER_ROW; k++) {
            int t = trow * TILES_PER_ROW + k;
            uint32_t *o = out + k * c->tw;
            if (t >= c->ntiles) { memset(o, 0, (size_t)c->tw * 4); continue; }
            const uint8_t *src = s->raw + ((size_t)t * c->tw * c->th + (size_t)py * c->tw) * 2;
            for (int x = 0; x < c->tw; x++) o[x] = argb1555_to_rgba(rd16(src + x * 2));
        }
    }
}

/* (re)build a pack cblock's texture from its blob; parse = also read the cell grid (first time) */
static bool cblock_build(CBlock *c, bool parse)
{
#ifdef PLAT_BAKED_ASSETS
    int bw, bh; uint8_t *meta = NULL;
    if ((c->tex = baked_tex(c->id, &bw, &bh, parse ? &meta : NULL))) {
        if (parse) {   /* u16 frames, cols, rows, tw, th, ntiles, sheet_cols, 0, then the cells */
            if (!meta) return false;
            c->frames = rd16(meta); c->cols = rd16(meta + 2); c->rows = rd16(meta + 4);
            c->tw = rd16(meta + 6); c->th = rd16(meta + 8); c->ntiles = rd16(meta + 10); c->sheet_cols = rd16(meta + 12);
            int n = c->frames * c->cols * c->rows;
            uint16_t *cells = malloc((size_t)n * 2 + 2);
            if (cells) for (int i = 0; i < n; i++) cells[i] = rd16(meta + 16 + i * 2);
            c->cells = cells;
            free(meta);
            if (!cells) return false;
        }
        c->last_used = g_frame;
        return true;
    }
#endif
    const PackEntry *e = packs_find_type(c->id, RES_CBLOCK);
    if (!e) { fprintf(stderr, "cblock %08X not found\n", c->id); return false; }
    const uint8_t *d = e->data;
    int n = rd16(d) * rd16(d + 2) * rd16(d + 4), extra = rd16(d + 6);
    if (parse) {
        c->frames = rd16(d); c->cols = rd16(d + 2); c->rows = rd16(d + 4);
        uint16_t *cells = malloc((size_t)n * 2 + 2); uint8_t *mask = malloc((size_t)extra + 1);
        if (!cells || !mask) { free(cells); free(mask); return false; }
        memcpy(cells, d + 16, (size_t)n * 2);
        le16_to_host(cells, (size_t)n);
        memcpy(mask, d + 16 + n * 2, (size_t)extra);
        c->cells = cells; c->mask = mask;
    }
    const uint8_t *bank = d + 16 + n * 2 + extra;
    c->tw = rd16(bank); c->th = rd16(bank + 2); c->ntiles = rd16(bank + 6);
    uint32_t csz = rd32(bank + 8), dsz = rd32(bank + 12);
    const uint8_t *comp = bank + 16;
    uint8_t *raw = malloc(dsz + 16);
    bool ok = raw != NULL;
    if (ok && csz == dsz) memcpy(raw, comp, dsz);
    else if (ok && lzo1z_decompress(comp, csz, raw, dsz + 16) < 0) { fprintf(stderr, "cblock %08X: bad lzo\n", c->id); ok = false; }
#ifdef PLAT_LOW_MEMORY
    packs_release(c->id);   /* the tiles are unpacked; the blob can go */
#endif
    if (ok) {
        c->sheet_cols = TILES_PER_ROW;
        int srows = (c->ntiles + TILES_PER_ROW - 1) / TILES_PER_ROW;
        SheetSrc src = { c, raw };
        c->tex = finish_tex(rtex_create_rows(R, TILES_PER_ROW * c->tw, srows * c->th, sheet_rows, &src), c->id, TILES_PER_ROW * c->tw, srows * c->th);
        ok = c->tex != NULL;
    }
    free(raw);
    c->last_used = g_frame;
    return ok;
}

CBlock *cblock_get(uint32_t id)
{
    for (int i = 0; i < g_ncb; i++) if (g_cb[i].id == id) return &g_cb[i];
    if (g_ncb == MAX_CB) { fprintf(stderr, "cblock %08X: cache full\n", id); return NULL; }
    CBlock *c = &g_cb[g_ncb];
    memset(c, 0, sizeof *c);
    c->id = id; c->from_pack = true;
    if (!cblock_build(c, true)) { free((void *)c->cells); free((void *)c->mask); memset(c, 0, sizeof *c); return NULL; }
    g_ncb++;
    return c;
}

/* a one-frame cblock over a w x h sheet of tw x th cells, cell i = tile i */
static CBlock *cblock_sheet(uint32_t id, RTex *tex, int w, int h, int tw, int th)
{
    if (!tex) return NULL;
    if (g_ncb == MAX_CB) { rtex_destroy(tex); return NULL; }
    CBlock *c = &g_cb[g_ncb];
    memset(c, 0, sizeof *c);
    c->id = id; c->frames = 1; c->cols = w / tw; c->rows = h / th; c->tw = tw; c->th = th;
    c->ntiles = c->cols * c->rows;
    uint16_t *cells = malloc((size_t)c->ntiles * 2 + 2);
    if (!cells) { rtex_destroy(tex); return NULL; }
    for (int i = 0; i < c->ntiles; i++) cells[i] = (uint16_t)i;
    c->cells = cells; c->mask = NULL;
    c->sheet_cols = c->cols;          /* the sheet is the image itself */
    c->tex = tex;
    c->last_used = g_frame;
    g_ncb++;
    return c;
}

CBlock *cblock_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int tw, int th)
{
    for (int i = 0; i < g_ncb; i++) if (g_cb[i].id == id) return &g_cb[i];
    if (g_ncb == MAX_CB) return NULL;
    return cblock_sheet(id, make_tex(id, w, h, px), w, h, tw, th);
}

CBlock *cblock_from_png(uint32_t id, const char *path, int tw, int th)
{
    for (int i = 0; i < g_ncb; i++) if (g_cb[i].id == id) return &g_cb[i];
    int w, h; RTex *t = gfx_image_tex(path, &w, &h);
    if (t) rtex_set_tag(t, id);
    CBlock *c = cblock_sheet(id, t, w, h, tw, th);
    if (c) c->file = strdup(path);
    return c;
}

RTex *cblock_tex(const CBlock *cc)
{
    if (!cc) return NULL;
    CBlock *c = (CBlock *)cc;
    if (!c->tex && c->from_pack) cblock_build(c, false);
    else if (!c->tex && c->file) { int w, h; if ((c->tex = gfx_image_tex(c->file, &w, &h))) rtex_set_tag(c->tex, c->id); }
    c->last_used = g_frame;
    return c->tex;
}

void cblock_unload(const CBlock *cc)
{
    CBlock *c = (CBlock *)cc;
    if (c && c->tex && (c->from_pack || c->file)) { rtex_destroy(c->tex); c->tex = NULL; }
}

int cblock_ncells(const CBlock *c) { return c->frames * c->cols * c->rows; }

void cblock_tint(const CBlock *c, uint8_t r, uint8_t g, uint8_t b)
{
    RTex *t = cblock_tex(c);
    if (t) rtex_set_color_mod(t, r, g, b);
}

void cblock_draw_tile(const CBlock *c, int t, float x, float y, bool flip)
{
    if (t < 0 || t >= c->ntiles) return;
    RTex *tex = cblock_tex(c);
    RFRect src = { (float)((t % c->sheet_cols) * c->tw), (float)((t / c->sheet_cols) * c->th), (float)c->tw, (float)c->th };
    RFRect dst = { x, y, (float)c->tw, (float)c->th };
    if (flip) r_tex_rot(R, tex, &src, &dst, 0, NULL, R_FLIP_H);
    else r_tex(R, tex, &src, &dst);
}

/* ---- tile batches: one r_tex_batch for a run of tiles out of one bank ---- */
#define BATCH_MAX 512
static struct { const CBlock *c; RTex *tex; int n, shift; RFRect src[BATCH_MAX], dst[BATCH_MAX]; } g_batch;

static void batch_flush(void)
{
    if (g_batch.n && g_batch.tex) r_tex_batch(R, g_batch.tex, g_batch.src, g_batch.dst, g_batch.n);
    g_batch.n = 0;
}
void cblock_batch_begin(const CBlock *c)
{
    batch_flush();
    g_batch.c = c; g_batch.tex = cblock_tex(c);
    int cols = c->sheet_cols;   /* the sheet's width in tiles: a shift instead of a division when it is a power of two */
    g_batch.shift = cols > 0 && !(cols & (cols - 1)) ? __builtin_ctz((unsigned)cols) : -1;
}
void cblock_batch_tile(int t, float x, float y, bool flip)
{
    const CBlock *c = g_batch.c;
    if (!c || t < 0 || t >= c->ntiles) return;
    if (g_batch.n == BATCH_MAX) batch_flush();
    int col = g_batch.shift >= 0 ? t & (c->sheet_cols - 1) : t % c->sheet_cols, row = g_batch.shift >= 0 ? t >> g_batch.shift : t / c->sheet_cols;
    g_batch.src[g_batch.n] = (RFRect){ (float)(col * c->tw), (float)(row * c->th), (float)c->tw, (float)c->th };
    g_batch.dst[g_batch.n] = (RFRect){ x, y, flip ? -(float)c->tw : (float)c->tw, (float)c->th };
    g_batch.n++;
}
void cblock_batch_end(void) { batch_flush(); g_batch.c = NULL; g_batch.tex = NULL; }

void cblock_draw_frame(const CBlock *c, int frame, float x, float y, bool flip)
{
    if (frame < 0 || frame >= c->frames) return;
    const uint16_t *cells = c->cells + frame * c->cols * c->rows;
    cblock_batch_begin(c);
    for (int r = 0; r < c->rows; r++)
        for (int col = 0; col < c->cols; col++) {
            uint16_t t = cells[r * c->cols + col];
            if (t == 0xFFFF) continue;
            int cx = flip ? (c->cols - 1 - col) : col;
            cblock_batch_tile(t, x + cx * c->tw, y + r * c->th, flip);
        }
    cblock_batch_end();
}

/* ---- sprites ---- */
#define MAX_SPR 128
static Sprite g_spr[MAX_SPR]; static int g_nspr;
static int pot(int n) { int p = 1; while (p < n) p <<= 1; return p; }

/* frames side by side, each the top-left w x h of its POT-padded frame */
typedef struct { const Sprite *s; const uint8_t *raw; int pw, ph, fmt; } StripSrc;
static void strip_rows(void *ud, int y0, int n, uint32_t *out)
{
    const StripSrc *ss = ud; const Sprite *s = ss->s;
    int W = s->w * s->frames;
    for (int y = y0; y < y0 + n; y++, out += W)
        for (int f = 0; f < s->frames; f++) {
            const uint8_t *src = ss->raw + ((size_t)f * ss->pw * ss->ph + (size_t)y * ss->pw) * 2;
            uint32_t *o = out + f * s->w;
            if (ss->fmt == 3) for (int x = 0; x < s->w; x++) o[x] = argb4444_to_rgba(rd16(src + x * 2));
            else for (int x = 0; x < s->w; x++) o[x] = argb1555_to_rgba(rd16(src + x * 2));
        }
}

#ifdef PLAT_BAKED_ASSETS
/* a pack sprite (or font glyph sprite) baked for the console; first = also take its size and frame count */
static bool sprite_baked(Sprite *s, bool first)
{
    int bw, bh; uint8_t *meta = NULL;
    s->tex = baked_tex(s->id, &bw, &bh, first ? &meta : NULL);
    if (!s->tex) return false;
    if (first) {   /* u16 frame w, h, frames */
        if (!meta) { rtex_destroy(s->tex); s->tex = NULL; return false; }
        s->w = rd16(meta); s->h = rd16(meta + 2); s->frames = rd16(meta + 4);
        free(meta);
    }
    s->last_used = g_frame;
    return true;
}
#endif

/* the texture of a sprite blob; first = also read its size and frame count */
static bool sprite_build(Sprite *s, const uint8_t *d, uint32_t size, bool first)
{
    int w = rd16(d), h = rd16(d + 2), frames = rd16(d + 6);
    int fmt = rd16(d + 4);   /* 1 = ARGB1555, 3 = ARGB4444 (translucent dialog boxes) */
    uint32_t dsz = rd32(d + 12);
    int pw = pot(w), ph = pot(h);
    uint8_t *raw = malloc(dsz + 16);
    if (!raw) return false;
    uint32_t csz = size - 16;
    if (csz == dsz) memcpy(raw, d + 16, dsz);
    else if (lzo1z_decompress(d + 16, csz, raw, dsz + 16) < 0) { fprintf(stderr, "sprite %08X: bad lzo\n", s->id); free(raw); return false; }
    if (first) {
        s->w = w; s->h = h; s->frames = frames < 1 ? 1 : frames;
        if ((uint32_t)(s->frames * pw * ph * 2) != dsz && (pw * ph * 2) > 0) s->frames = dsz / (pw * ph * 2);
    }
    StripSrc src = { s, raw, pw, ph, fmt };
    s->tex = finish_tex(rtex_create_rows(R, s->w * s->frames, s->h, strip_rows, &src), s->id, s->w * s->frames, s->h);
    free(raw);
    s->last_used = g_frame;
    return s->tex != NULL;
}

Sprite *sprite_from_blob(uint32_t id, const uint8_t *d, uint32_t size)
{
    if (g_nspr == MAX_SPR) return NULL;
    Sprite *s = &g_spr[g_nspr]; memset(s, 0, sizeof *s);
    s->id = id;
#ifdef PLAT_BAKED_ASSETS
    if (sprite_baked(s, true)) { g_nspr++; return s; }
#endif
    if (!sprite_build(s, d, size, true)) return NULL;
    g_nspr++;
    return s;
}

/* a sprite of `frames` frames side by side over a w x h texture */
static Sprite *sprite_strip(uint32_t id, RTex *tex, int w, int h, int frames)
{
    if (!tex) return NULL;
    if (g_nspr == MAX_SPR || frames < 1) { rtex_destroy(tex); return NULL; }
    Sprite *s = &g_spr[g_nspr]; memset(s, 0, sizeof *s);
    s->id = id; s->w = w / frames; s->h = h; s->frames = frames;
    s->tex = tex;
    s->last_used = g_frame;
    g_nspr++;
    return s;
}

Sprite *sprite_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int frames)
{
    for (int i = 0; i < g_nspr; i++) if (g_spr[i].id == id) return &g_spr[i];
    if (g_nspr == MAX_SPR || frames < 1) return NULL;
    return sprite_strip(id, make_tex(id, w, h, px), w, h, frames);
}

Sprite *sprite_from_png(uint32_t id, const char *path, int frame_w)
{
    for (int i = 0; i < g_nspr; i++) if (g_spr[i].id == id) return &g_spr[i];
    int w, h; RTex *t = gfx_image_tex(path, &w, &h);
    if (t) rtex_set_tag(t, id);
    Sprite *s = t ? sprite_strip(id, t, w, h, frame_w > 0 && w >= frame_w ? w / frame_w : 1) : NULL;
    if (s) s->file = strdup(path);
    return s;
}

Sprite *sprite_get(uint32_t id)
{
    for (int i = 0; i < g_nspr; i++) if (g_spr[i].id == id) return &g_spr[i];
#ifdef PLAT_BAKED_ASSETS
    if (g_nspr < MAX_SPR && packs_peek_type(id, RES_TEX)) {
        Sprite *s = &g_spr[g_nspr]; memset(s, 0, sizeof *s);
        s->id = id;
        if (sprite_baked(s, true)) { s->from_pack = true; g_nspr++; return s; }
    }
#endif
    const PackEntry *e = packs_find_type(id, RES_SPRITE);
    if (!e) { fprintf(stderr, "sprite %08X not found\n", id); return NULL; }
    Sprite *s = sprite_from_blob(id, e->data, e->size);
#ifdef PLAT_LOW_MEMORY
    packs_release(id);
#endif
    if (s) s->from_pack = true;
    return s;
}

RTex *sprite_tex(const Sprite *cs)
{
    if (!cs) return NULL;
    Sprite *s = (Sprite *)cs;
    if (!s->tex && s->from_pack) {
#ifdef PLAT_BAKED_ASSETS
        if (sprite_baked(s, false)) { s->last_used = g_frame; return s->tex; }
#endif
        const PackEntry *e = packs_find_type(s->id, RES_SPRITE);
        if (e) sprite_build(s, e->data, e->size, false);
#ifdef PLAT_LOW_MEMORY
        packs_release(s->id);
#endif
    } else if (!s->tex && s->file) { int w, h; if ((s->tex = gfx_image_tex(s->file, &w, &h))) rtex_set_tag(s->tex, s->id); }
    s->last_used = g_frame;
    return s->tex;
}

/* ---- memory pressure: drop the pack (or PNG) texture drawn longest ago (not in this frame or the one being rendered) ---- */
static bool evict_one(void)
{
    uint32_t best = g_frame - 1; RTex **victim = NULL;
    for (int i = 0; i < g_nspr; i++) if ((g_spr[i].from_pack || g_spr[i].file) && g_spr[i].tex && g_spr[i].last_used < best) { best = g_spr[i].last_used; victim = &g_spr[i].tex; }
    for (int i = 0; i < g_ncb; i++) if ((g_cb[i].from_pack || g_cb[i].file) && g_cb[i].tex && g_cb[i].last_used < best) { best = g_cb[i].last_used; victim = &g_cb[i].tex; }
    if (!victim) return false;
    rtex_destroy(*victim); *victim = NULL;
    return true;
}

/* a pack block that doesn't fit (a level after the menus): any texture, drawn longest ago first. A backend whose GPU may
 * still be drawing it keeps its video memory until then (the Saturn's render_sat.c does). */
static bool evict_any(void)
{
    if (evict_one()) return true;
    uint32_t best = UINT32_MAX; RTex **victim = NULL;
    for (int i = 0; i < g_nspr; i++) if ((g_spr[i].from_pack || g_spr[i].file) && g_spr[i].tex && g_spr[i].last_used <= best) { best = g_spr[i].last_used; victim = &g_spr[i].tex; }
    for (int i = 0; i < g_ncb; i++) if ((g_cb[i].from_pack || g_cb[i].file) && g_cb[i].tex && g_cb[i].last_used <= best) { best = g_cb[i].last_used; victim = &g_cb[i].tex; }
    if (!victim) return false;
    rtex_destroy(*victim); *victim = NULL;
    return true;
}

bool gfx_init(Ren *r) { R = r; r_set_evict_hook(evict_one); packs_set_evict_hook(evict_any); return true; }

void gfx_flush(void)
{
    for (int i = 0; i < g_nspr; i++) { rtex_destroy(g_spr[i].tex); free((void *)g_spr[i].file); }
    for (int i = 0; i < g_ncb; i++) { rtex_destroy(g_cb[i].tex); free((void *)g_cb[i].cells); free((void *)g_cb[i].mask); free((void *)g_cb[i].file); }
    g_nspr = g_ncb = 0;
}
void gfx_frame(void) { g_frame++; }

void gfx_scanlines(int sw, int sh)
{
    static RFRect rows[512];
    int n = 0;
    for (int y = 1; y < sh && n < 512; y += 2) rows[n++] = (RFRect){ 0, (float)y, (float)sw, 1 };
    r_set_draw_blend(R, R_BLEND_BLEND); r_set_draw_color(R, 0, 0, 0, 70);
    r_fill_rects(R, rows, n);
}

void sprite_draw(const Sprite *s, int frame, float x, float y, bool flip)
{
    if (!s) return;
    if (frame < 0 || frame >= s->frames) frame = 0;
    RTex *t = sprite_tex(s);
    RFRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    RFRect dst = { x, y, (float)s->w, (float)s->h };
    if (flip) r_tex_rot(R, t, &src, &dst, 0, NULL, R_FLIP_H);
    else r_tex(R, t, &src, &dst);
}
void sprite_draw_scaled(const Sprite *s, int frame, float x, float y, float w, float h)
{
    if (!s) return;
    RFRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    RFRect dst = { x, y, w, h };
    r_tex(R, sprite_tex(s), &src, &dst);
}

void sprite_draw_scaled_mod(const Sprite *s, int frame, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (!s) return;
    RTex *t = sprite_tex(s);
    rtex_set_color_mod(t, r, g, b); rtex_set_alpha_mod(t, alpha);
    sprite_draw_scaled(s, frame, x, y, w, h);
    rtex_set_color_mod(t, 255, 255, 255); rtex_set_alpha_mod(t, 255);
}

void sprite_draw_rotated(const Sprite *s, int frame, float cx, float cy, float scale, float angle, uint8_t bright, uint8_t alpha)
{
    if (!s) return;
    RTex *t = sprite_tex(s);
    RFRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    RFRect dst = { cx - s->w * scale * 0.5f, cy - s->h * scale * 0.5f, s->w * scale, s->h * scale };
    rtex_set_color_mod(t, bright, bright, bright); rtex_set_alpha_mod(t, alpha);
    r_tex_rot(R, t, &src, &dst, angle, NULL, R_FLIP_NONE);
    rtex_set_color_mod(t, 255, 255, 255); rtex_set_alpha_mod(t, 255);
}
void sprite_draw_mod(const Sprite *s, int frame, float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (!s) return;
    RTex *t = sprite_tex(s);
    rtex_set_color_mod(t, r, g, b); rtex_set_alpha_mod(t, alpha);
    sprite_draw(s, frame, x, y, false);
    rtex_set_color_mod(t, 255, 255, 255); rtex_set_alpha_mod(t, 255);
}
