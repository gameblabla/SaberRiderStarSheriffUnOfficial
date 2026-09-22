#include "gfx.h"
#include "pack.h"
#include "lzo1z.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TILES_PER_ROW 32
static SDL_Renderer *R;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool gfx_init(SDL_Renderer *r) { R = r; return true; }

static void argb1555_to_rgba(const uint8_t *src, uint32_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t v = rd16(src + i * 2);
        uint32_t a = (v & 0x8000) ? 255 : 0;
        uint32_t rr = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        rr = (rr * 255 + 15) / 31; g = (g * 255 + 15) / 31; b = (b * 255 + 15) / 31;
        dst[i] = a << 24 | b << 16 | g << 8 | rr;   /* SDL_PIXELFORMAT_ABGR8888 in memory = RGBA bytes */
    }
}

static void argb4444_to_rgba(const uint8_t *src, uint32_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t v = rd16(src + i * 2);
        uint32_t a = (v >> 12) * 17, rr = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17;
        dst[i] = a << 24 | b << 16 | g << 8 | rr;
    }
}

static SDL_Texture *make_tex(int w, int h, const uint32_t *px)
{
    SDL_Texture *t = SDL_CreateTexture(R, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) { fprintf(stderr, "tex: %s\n", SDL_GetError()); return NULL; }
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return t;
}

/* ---- cblock cache ---- */
#define MAX_CB 64
static CBlock g_cb[MAX_CB]; static int g_ncb;

CBlock *cblock_get(uint32_t id)
{
    for (int i = 0; i < g_ncb; i++) if (g_cb[i].id == id) return &g_cb[i];
    const PackEntry *e = packs_find_type(id, RES_CBLOCK);
    if (!e || g_ncb == MAX_CB) { fprintf(stderr, "cblock %08X not found\n", id); return NULL; }
    const uint8_t *d = e->data;
    CBlock *c = &g_cb[g_ncb];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->frames = rd16(d); c->cols = rd16(d + 2); c->rows = rd16(d + 4);
    int extra = rd16(d + 6);
    int n = c->frames * c->cols * c->rows;
    c->cells = (const uint16_t *)(d + 16);        /* little-endian host assumed */
    c->mask = d + 16 + n * 2;
    const uint8_t *bank = c->mask + extra;
    c->tw = rd16(bank); c->th = rd16(bank + 2); c->ntiles = rd16(bank + 6);
    uint32_t csz = rd32(bank + 8), dsz = rd32(bank + 12);
    const uint8_t *comp = bank + 16;
    uint8_t *raw = malloc(dsz + 16);
    if (csz == dsz) memcpy(raw, comp, dsz);
    else if (lzo1z_decompress(comp, csz, raw, dsz + 16) < 0) { fprintf(stderr, "cblock %08X: bad lzo\n", id); free(raw); return NULL; }
    /* build sheet */
    c->sheet_cols = TILES_PER_ROW;
    int srows = (c->ntiles + TILES_PER_ROW - 1) / TILES_PER_ROW;
    int W = TILES_PER_ROW * c->tw, H = srows * c->th;
    uint32_t *px = calloc((size_t)W * H, 4);
    uint32_t *tile = malloc((size_t)c->tw * c->th * 4);
    for (int t = 0; t < c->ntiles; t++) {
        argb1555_to_rgba(raw + (size_t)t * c->tw * c->th * 2, tile, c->tw * c->th);
        int ox = (t % TILES_PER_ROW) * c->tw, oy = (t / TILES_PER_ROW) * c->th;
        for (int y = 0; y < c->th; y++) memcpy(px + (size_t)(oy + y) * W + ox, tile + (size_t)y * c->tw, c->tw * 4);
    }
    c->tex = make_tex(W, H, px);
    free(px); free(tile); free(raw);
    g_ncb++;
    return c;
}

CBlock *cblock_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int tw, int th)
{
    for (int i = 0; i < g_ncb; i++) if (g_cb[i].id == id) return &g_cb[i];
    if (g_ncb == MAX_CB) return NULL;
    CBlock *c = &g_cb[g_ncb];
    memset(c, 0, sizeof *c);
    c->id = id; c->frames = 1; c->cols = w / tw; c->rows = h / th; c->tw = tw; c->th = th;
    c->ntiles = c->cols * c->rows;
    uint16_t *cells = malloc((size_t)c->ntiles * 2);
    for (int i = 0; i < c->ntiles; i++) cells[i] = (uint16_t)i;
    c->cells = cells; c->mask = NULL;
    c->sheet_cols = c->cols;          /* the sheet is the image itself */
    c->tex = make_tex(w, h, px);
    if (!c->tex) { free(cells); return NULL; }
    g_ncb++;
    return c;
}

int cblock_ncells(const CBlock *c) { return c->frames * c->cols * c->rows; }

void cblock_tint(const CBlock *c, uint8_t r, uint8_t g, uint8_t b)
{
    if (c && c->tex) SDL_SetTextureColorMod(c->tex, r, g, b);
}

void cblock_draw_tile(const CBlock *c, int t, float x, float y, bool flip)
{
    if (t < 0 || t >= c->ntiles) return;
    SDL_FRect src = { (float)((t % c->sheet_cols) * c->tw), (float)((t / c->sheet_cols) * c->th), (float)c->tw, (float)c->th };
    SDL_FRect dst = { x, y, (float)c->tw, (float)c->th };
    if (flip) SDL_RenderTextureRotated(R, c->tex, &src, &dst, 0, NULL, SDL_FLIP_HORIZONTAL);
    else SDL_RenderTexture(R, c->tex, &src, &dst);
}

void cblock_draw_frame(const CBlock *c, int frame, float x, float y, bool flip)
{
    if (frame < 0 || frame >= c->frames) return;
    const uint16_t *cells = c->cells + frame * c->cols * c->rows;
    for (int r = 0; r < c->rows; r++)
        for (int col = 0; col < c->cols; col++) {
            uint16_t t = cells[r * c->cols + col];
            if (t == 0xFFFF) continue;
            int cx = flip ? (c->cols - 1 - col) : col;
            cblock_draw_tile(c, t, x + cx * c->tw, y + r * c->th, flip);
        }
}

/* ---- sprites ---- */
#define MAX_SPR 128
static Sprite g_spr[MAX_SPR]; static int g_nspr;
static int pot(int n) { int p = 1; while (p < n) p <<= 1; return p; }

Sprite *sprite_from_blob(uint32_t id, const uint8_t *d, uint32_t size)
{
    if (g_nspr == MAX_SPR) return NULL;
    Sprite *s = &g_spr[g_nspr]; memset(s, 0, sizeof *s);
    struct { const uint8_t *data; uint32_t size; } ev = { d, size }; const void *e = &ev;
    (void)e;
    s->id = id; s->w = rd16(d); s->h = rd16(d + 2); s->frames = rd16(d + 6);
    int fmt = rd16(d + 4);   /* 1 = ARGB1555, 3 = ARGB4444 (translucent dialog boxes) */
    uint32_t dsz = rd32(d + 12);
    int pw = pot(s->w), ph = pot(s->h);
    uint8_t *raw = malloc(dsz + 16);
    uint32_t csz = size - 16;
    if (csz == dsz) memcpy(raw, d + 16, dsz);
    else if (lzo1z_decompress(d + 16, csz, raw, dsz + 16) < 0) { fprintf(stderr, "sprite %08X: bad lzo\n", id); free(raw); return NULL; }
    if (s->frames < 1) s->frames = 1;
    if ((uint32_t)(s->frames * pw * ph * 2) != dsz && (pw * ph * 2) > 0) s->frames = dsz / (pw * ph * 2);
    int W = s->w * s->frames, H = s->h;
    uint32_t *px = calloc((size_t)W * H, 4);
    uint32_t *fr = malloc((size_t)pw * ph * 4);
    for (int f = 0; f < s->frames; f++) {
        if (fmt == 3) argb4444_to_rgba(raw + (size_t)f * pw * ph * 2, fr, pw * ph);
        else argb1555_to_rgba(raw + (size_t)f * pw * ph * 2, fr, pw * ph);
        for (int y = 0; y < s->h; y++) memcpy(px + (size_t)y * W + f * s->w, fr + (size_t)y * pw, s->w * 4);
    }
    s->tex = make_tex(W, H, px);
    free(px); free(fr); free(raw);
    g_nspr++;
    return s;
}

Sprite *sprite_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int frames)
{
    for (int i = 0; i < g_nspr; i++) if (g_spr[i].id == id) return &g_spr[i];
    if (g_nspr == MAX_SPR || frames < 1) return NULL;
    Sprite *s = &g_spr[g_nspr]; memset(s, 0, sizeof *s);
    s->id = id; s->w = w / frames; s->h = h; s->frames = frames;
    s->tex = make_tex(w, h, px);
    if (!s->tex) return NULL;
    g_nspr++;
    return s;
}

Sprite *sprite_get(uint32_t id)
{
    for (int i = 0; i < g_nspr; i++) if (g_spr[i].id == id) return &g_spr[i];
    const PackEntry *e = packs_find_type(id, RES_SPRITE);
    if (!e) { fprintf(stderr, "sprite %08X not found\n", id); return NULL; }
    return sprite_from_blob(id, e->data, e->size);
}

void sprite_draw(const Sprite *s, int frame, float x, float y, bool flip)
{
    if (!s) return;
    if (frame < 0 || frame >= s->frames) frame = 0;
    SDL_FRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    SDL_FRect dst = { x, y, (float)s->w, (float)s->h };
    if (flip) SDL_RenderTextureRotated(R, s->tex, &src, &dst, 0, NULL, SDL_FLIP_HORIZONTAL);
    else SDL_RenderTexture(R, s->tex, &src, &dst);
}
void sprite_draw_scaled(const Sprite *s, int frame, float x, float y, float w, float h)
{
    if (!s) return;
    SDL_FRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    SDL_FRect dst = { x, y, w, h };
    SDL_RenderTexture(R, s->tex, &src, &dst);
}

void sprite_draw_scaled_mod(const Sprite *s, int frame, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (!s) return;
    SDL_SetTextureColorMod(s->tex, r, g, b); SDL_SetTextureAlphaMod(s->tex, alpha);
    sprite_draw_scaled(s, frame, x, y, w, h);
    SDL_SetTextureColorMod(s->tex, 255, 255, 255); SDL_SetTextureAlphaMod(s->tex, 255);
}

void sprite_draw_rotated(const Sprite *s, int frame, float cx, float cy, float scale, float angle, uint8_t bright, uint8_t alpha)
{
    if (!s) return;
    SDL_FRect src = { (float)(frame * s->w), 0, (float)s->w, (float)s->h };
    SDL_FRect dst = { cx - s->w * scale * 0.5f, cy - s->h * scale * 0.5f, s->w * scale, s->h * scale };
    SDL_SetTextureColorMod(s->tex, bright, bright, bright); SDL_SetTextureAlphaMod(s->tex, alpha);
    SDL_RenderTextureRotated(R, s->tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
    SDL_SetTextureColorMod(s->tex, 255, 255, 255); SDL_SetTextureAlphaMod(s->tex, 255);
}
void sprite_draw_mod(const Sprite *s, int frame, float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    if (!s) return;
    SDL_SetTextureColorMod(s->tex, r, g, b); SDL_SetTextureAlphaMod(s->tex, alpha);
    sprite_draw(s, frame, x, y, false);
    SDL_SetTextureColorMod(s->tex, 255, 255, 255); SDL_SetTextureAlphaMod(s->tex, 255);
}
