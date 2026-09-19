#include "dialog.h"
#include "pack.h"
#include "gfx.h"
#include "font.h"
#include "audio.h"
#include "namehash.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

enum { COL_WHITE, COL_GREEN, COL_PURPLE, COL_RED };
static const uint32_t TILESET[4] = { 0x8D39AA67, 0x84652CBC, 0xA2122E71, 0x1495B0AB };
static const uint8_t TEXTRGB[4][3] = { {255,255,255}, {160,255,160}, {255,160,255}, {255,120,120} };

bool dialog_open(Dialog *d, uint32_t text_id)
{
    memset(d, 0, sizeof *d);
    const PackEntry *e = packs_find(text_id);
    if (!e) return false;
    char buf[2048]; size_t n = e->size < sizeof buf - 1 ? e->size : sizeof buf - 1;
    memcpy(buf, e->data, n); buf[n] = 0;
    /* first line: optional sfx name */
    char *p = buf, *nl = strchr(p, '\n');
    if (nl) {
        *nl = 0;
        char *q = p; while (*q == ' ' || *q == '\r') q++;
        if (*q && strncmp(q, "<<", 2) != 0 && strncmp(q, "<|", 2) != 0 && strncmp(q, "</", 2) != 0) {
            size_t l = strlen(q); while (l && (q[l-1] == '\r' || q[l-1] == ' ')) q[--l] = 0;
            uint32_t sid = namehash(q);
            (void)sid; sfx_play_id(sid);
            p = nl + 1;
        } else { *nl = '\n'; }
    }
    DialogPage *pg = NULL;
    int color = COL_WHITE; uint32_t avatar = 0;
    char *line = p;
    while (line && *line) {
        char *end = strchr(line, '\n'); if (end) *end = 0;
        size_t l = strlen(line); while (l && line[l-1] == '\r') line[--l] = 0;
        if (!strncmp(line, "<<>>", 4)) { pg = NULL; }
        else if (!strncmp(line, "<|", 2)) {
            if (strstr(line, "GREEN")) color = COL_GREEN; else if (strstr(line, "PURPLE")) color = COL_PURPLE;
            else if (strstr(line, "RED")) color = COL_RED; else color = COL_WHITE;
        } else if (!strncmp(line, "</", 2)) {
            char name[64] = {0}; sscanf(line, "</%63[^/]/>", name); avatar = namehash(name);
        } else if (l) {
            if (!pg) {
                if (d->npages == DLG_MAX_PAGES) break;
                pg = &d->pages[d->npages++]; pg->color = color; pg->avatar_id = avatar; pg->text[0] = 0;
            }
            /* "<r>" -> newline */
            char tmp[512]; int o = 0;
            for (char *s = line; *s && o < 500; ) {
                if (!strncmp(s, "<r>", 3)) { tmp[o++] = '\n'; s += 3; } else tmp[o++] = *s++;
            }
            tmp[o] = 0;
            if (pg->text[0]) strncat(pg->text, "\n", sizeof pg->text - strlen(pg->text) - 1);
            strncat(pg->text, tmp, sizeof pg->text - strlen(pg->text) - 1);
        }
        line = end ? end + 1 : NULL;
    }
    d->active = d->npages > 0;
    return d->active;
}

void dialog_update(Dialog *d, const Input *in, float dt)
{
    if (!d->active) return;
    if (d->frame < 21) { d->frame++; return; }
    const DialogPage *pg = &d->pages[d->page];
    size_t total = strlen(pg->text);
    d->chars += dt * 40.0f;   /* ~40 chars/s typewriter */
    bool press = btn_pressed(in, BTN_SHOOT) || btn_pressed(in, BTN_JUMP) || btn_pressed(in, BTN_PAUSE);
    if (press) {
        if (d->chars < (float)total) d->chars = (float)total;
        else if (++d->page >= d->npages) { d->active = false; }
        else d->chars = 0;
    }
}

/* word-wrapped text into lines of at most `maxw` pixels */
static int wrap(const Font *f, const char *text, int maxw, char lines[8][80])
{
    int n = 0, len = 0; lines[0][0] = 0;
    const char *s = text;
    while (*s && n < 8) {
        if (*s == '\n') { lines[n][len] = 0; n++; len = 0; if (n < 8) lines[n][0] = 0; s++; continue; }
        const char *w = s; while (*w && *w != ' ' && *w != '\n') w++;
        int wl = (int)(w - s);
        char word[80]; int wn = wl < 79 ? wl : 79; memcpy(word, s, wn); word[wn] = 0;
        char cand[80]; snprintf(cand, sizeof cand, "%s%s%s", lines[n], len ? " " : "", word);
        if (len && font_text_width(f, cand) > maxw) { lines[n][len] = 0; n++; len = 0; if (n >= 8) break; lines[n][0] = 0; continue; }
        strcpy(lines[n], cand); len = (int)strlen(cand);
        s = w; while (*s == ' ') s++;
    }
    if (n < 8) { lines[n][len] = 0; n++; }
    return n;
}

static void draw_box(SDL_Renderer *r, Sprite *ts, int x, int y, int w, int h)
{
    (void)r;
    if (!ts || ts->frames < 9) return;
    int t = ts->w;
    for (int yy = 0; yy < h; yy += t)
        for (int xx = 0; xx < w; xx += t) {
            int col = xx == 0 ? 0 : (xx + t >= w ? 2 : 1), row = yy == 0 ? 0 : (yy + t >= h ? 2 : 1);
            sprite_draw(ts, row * 3 + col, (float)(x + xx), (float)(y + yy), false);
        }
}

void dialog_draw(const Dialog *d, SDL_Renderer *r, int sw, int sh)
{
    if (!d->active) return;
    const DialogPage *pg = &d->pages[d->page];
    Font *f = font_get(0x12072E60);
    float open = d->frame < 21 ? d->frame / 21.0f : 1.0f;
    /* box x 76..350 (avatar) / text rect 100..350 x 176..224, avatar label at 1/3 scale top-left (offset 6,-8) */
    Sprite *av = pg->avatar_id ? sprite_get(pg->avatar_id) : NULL;
    int bx = av ? 72 : 72, by = 168, bw = 350 - bx + 6, bh = sh - 8 - by;
    int shown = (int)(bh * open);
    draw_box(r, sprite_get(TILESET[pg->color]), bx, by + (bh - shown), bw, shown);
    if (open < 1.0f || !f) return;
    if (av) sprite_draw(av, 0, (float)(bx + 6), (float)(by - 8), false);
    (void)sw;
    char lines[8][80]; int n = wrap(f, pg->text, av ? 228 : 260, lines);
    int remaining = (int)d->chars;
    for (int i = 0; i < n && remaining > 0; i++) {
        int l = (int)strlen(lines[i]);
        font_draw_n(f, lines[i], remaining < l ? remaining : l, (float)(av ? 116 : 84), (float)(176 + i * 10), TEXTRGB[pg->color][0], TEXTRGB[pg->color][1], TEXTRGB[pg->color][2]);
        remaining -= l + 1;
    }
    /* page indicator */
    Sprite *arrow = sprite_get(0x99993BFE);
    if (arrow && d->chars >= (float)strlen(pg->text)) sprite_draw(arrow, (SDL_GetTicks() / 200) % arrow->frames, (float)(bx + bw - 16), (float)(sh - 20), false);
}
