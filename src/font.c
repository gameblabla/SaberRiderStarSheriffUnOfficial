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
    rtex_set_color_mod(sprite_tex(f->spr), r, g, b);
    for (int i = 0; s[i] && i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int gi = c - 0x21;
        if (gi >= 0 && gi < f->spr->frames) sprite_draw(f->spr, gi, x, y, false);
        x += glyph_w(f, c);
    }
    rtex_set_color_mod(sprite_tex(f->spr), 255, 255, 255);
}
void font_draw(const Font *f, const char *s, float x, float y, uint8_t r, uint8_t g, uint8_t b) { font_draw_n(f, s, 1 << 30, x, y, r, g, b); }

void font_draw_scaled(const Font *f, const char *s, float x, float y, float scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (!f || !f->spr) return;
    rtex_set_color_mod(sprite_tex(f->spr), r, g, b);
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int gi = c - 0x21;
        if (gi >= 0 && gi < f->spr->frames) sprite_draw_scaled(f->spr, gi, x, y, f->spr->w * scale, f->spr->h * scale);
        x += glyph_w(f, c) * scale;
    }
    rtex_set_color_mod(sprite_tex(f->spr), 255, 255, 255);
}

int font_wrap(const Font *f, const char *text, float width, char out[][96], int max)
{
    int n = 0; char line[96] = ""; const char *p = text;
    while (*p && n < max) {
        const char *e = p; while (*e && *e != ' ') e++;
        char word[64]; int wl = (int)(e - p); if (wl > 63) wl = 63; memcpy(word, p, (size_t)wl); word[wl] = 0;
        char trial[96]; bool fits = snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word) < (int)sizeof trial;
        if (line[0] && (!fits || font_text_width(f, trial) > width)) { snprintf(out[n++], 96, "%s", line); snprintf(line, sizeof line, "%s", word); }
        else memcpy(line, trial, sizeof line);
        p = *e ? e + 1 : e;
    }
    if (line[0] && n < max) snprintf(out[n++], 96, "%s", line);
    return n;
}
