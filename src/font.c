#include "font.h"
#include "gfx.h"
#include "pack.h"
#include "lzo1z.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_FONTS 4
static Font g_fonts[MAX_FONTS]; static int g_nfonts;

Font *font_get(uint32_t id)
{
    for (int i = 0; i < g_nfonts; i++) if (g_fonts[i].id == id) return &g_fonts[i];
    const PackEntry *e = packs_find_type(id, RES_FONT);
    if (!e || g_nfonts == MAX_FONTS) return NULL;
    const uint8_t *d = e->data;
    Font *f = &g_fonts[g_nfonts];
    f->id = id; f->count = d[4] | d[5] << 8 | d[6] << 16 | d[7] << 24; f->widths = d + 8;
    /* the glyph sprite: reuse the sprite loader on the embedded blob by registering it under the font id */
    const uint8_t *blob = d + 8 + f->count * 2;
    f->spr = sprite_from_blob(id, blob, e->size - (8 + f->count * 2));
    f->h = f->spr ? f->spr->h : 8;
    g_nfonts++;
    return f;
}

static int glyph_w(const Font *f, unsigned char c)
{
    int i = c - 0x20;
    if (i < 0 || i >= f->count) return f->widths[0];
    return f->widths[i * 2] | f->widths[i * 2 + 1] << 8;
}

int font_text_width(const Font *f, const char *s)
{
    int w = 0; for (; *s; s++) w += glyph_w(f, (unsigned char)*s);
    return w;
}

int font_text_width_n(const Font *f, const char *s, int n)
{
    int w = 0; for (int i = 0; s[i] && i < n; i++) w += glyph_w(f, (unsigned char)s[i]);
    return w;
}

void font_draw_n(const Font *f, const char *s, int n, float x, float y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!f || !f->spr) return;
    SDL_SetTextureColorMod(f->spr->tex, r, g, b);
    for (int i = 0; s[i] && i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int gi = c - 0x21;
        if (gi >= 0 && gi < f->spr->frames) sprite_draw(f->spr, gi, x, y, false);
        x += glyph_w(f, c);
    }
    SDL_SetTextureColorMod(f->spr->tex, 255, 255, 255);
}
void font_draw(const Font *f, const char *s, float x, float y, uint8_t r, uint8_t g, uint8_t b) { font_draw_n(f, s, 1 << 30, x, y, r, g, b); }

void font_draw_scaled(const Font *f, const char *s, float x, float y, float scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (!f || !f->spr) return;
    SDL_SetTextureColorMod(f->spr->tex, r, g, b);
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int gi = c - 0x21;
        if (gi >= 0 && gi < f->spr->frames) sprite_draw_scaled(f->spr, gi, x, y, f->spr->w * scale, f->spr->h * scale);
        x += glyph_w(f, c) * scale;
    }
    SDL_SetTextureColorMod(f->spr->tex, 255, 255, 255);
}
