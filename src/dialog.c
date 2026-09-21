#include "dialog.h"
#include "pack.h"
#include "gfx.h"
#include "font.h"
#include "audio.h"
#include "namehash.h"
#include "heroes.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* box tilesets (ARGB4444): default/GREEN 8D39AA67, PURPLE A2122E71, RED 1495B0AB, BLUE 84652CBC (FUN_0042a5e0 matches "PU"/"RE"/"BL");
 * the text tint is the box's colour argument (0x80 = neutral): PU 80807468, BL 80807060, RE 80707480 */
enum { COL_GREEN = DLG_GREEN, COL_PURPLE = DLG_PURPLE, COL_RED = DLG_RED, COL_BLUE = DLG_BLUE, COL_WHITE = DLG_GREEN };
static const uint32_t TILESET[4] = { 0x8D39AA67, 0xA2122E71, 0x1495B0AB, 0x84652CBC };
static const uint8_t TEXTRGB[4][3] = { {255,255,255}, {255,232,208}, {224,232,255}, {255,224,192} };

static int g_hero = HERO_FIREBALL;
void dialog_set_hero(int character) { g_hero = character; }

#define AVATAR_FIREBALL 0x742F352A   /* dialog_avatar_fireball1 */
/* the level-1 script addresses Fireball by name and the other three heroes talk to him: playing as one of them
 * swaps the name and trades that hero's avatar (dialog_avatar_<hero>2) for Fireball's */
static const struct { int hero; const char *name; uint32_t avatar; } HERO_SWAP[] = {
    { HERO_SABER, "Saber Rider", 0x70719EF7 }, { HERO_APRIL, "April", 0x7AB49CB5 }, { HERO_COLT, "Colt", 0xE3E91860 },
};

static void replace_word(char *text, size_t cap, const char *from, const char *to)
{
    char *p;
    while ((p = strstr(text, from))) {
        size_t lf = strlen(from), lt = strlen(to), rest = strlen(p + lf);
        if (strlen(text) - lf + lt >= cap) return;
        memmove(p + lt, p + lf, rest + 1); memcpy(p, to, lt);
    }
}

static void adapt_pages(Dialog *d)
{
    for (int i = 0; i < d->npages; i++) {
        DialogPage *pg = &d->pages[i];
        for (size_t k = 0; k < sizeof HERO_SWAP / sizeof *HERO_SWAP; k++) {
            if (g_hero != HERO_SWAP[k].hero) continue;
            replace_word(pg->text, sizeof pg->text, "Fireball", HERO_SWAP[k].name);
            if (pg->avatar_id == HERO_SWAP[k].avatar) pg->avatar_id = AVATAR_FIREBALL;
            else if (pg->avatar_id == AVATAR_FIREBALL) pg->avatar_id = HERO_SWAP[k].avatar;
        }
    }
}

bool dialog_open(Dialog *d, uint32_t text_id)
{
    const PackEntry *e = packs_find(text_id);
    if (!e) { memset(d, 0, sizeof *d); return false; }
    char buf[2048]; size_t n = e->size < sizeof buf - 1 ? e->size : sizeof buf - 1;
    memcpy(buf, e->data, n); buf[n] = 0;
    return dialog_open_script(d, buf);
}

bool dialog_open_script(Dialog *d, const char *script)
{
    memset(d, 0, sizeof *d);
    char buf[4096]; snprintf(buf, sizeof buf, "%s", script);
    /* first line: optional sfx name */
    char *p = buf, *nl = strchr(p, '\n');
    if (nl) {
        *nl = 0;
        char *q = p; while (*q == ' ' || *q == '\r') q++;
        if (*q && strncmp(q, "<<", 2) != 0 && strncmp(q, "<|", 2) != 0 && strncmp(q, "</", 2) != 0) {
            size_t l = strlen(q); while (l && (q[l-1] == '\r' || q[l-1] == ' ')) q[--l] = 0;
            /* FUN_0042a5e0 only resolves the sample (DAT_00ac9aa0); FUN_0042b250 plays it on the first update, i.e. once
             * the camera has reached the focus point and the box opens, not when the trigger zone is entered */
            d->pending_sfx = namehash(q);
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
            const char *tag = line + 2;   /* the original compares the first two letters only: PU / RE / BL, anything else = green */
            if (!strncmp(tag, "PU", 2)) color = COL_PURPLE; else if (!strncmp(tag, "RE", 2)) color = COL_RED;
            else if (!strncmp(tag, "BL", 2)) color = COL_BLUE; else color = COL_GREEN;
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
    adapt_pages(d);
    return d->active;
}

bool dialog_open_text(Dialog *d, const char *text, int color)
{
    memset(d, 0, sizeof *d);
    DialogPage *pg = &d->pages[0];
    pg->color = color; snprintf(pg->text, sizeof pg->text, "%s", text);
    size_t l = strlen(pg->text); while (l && (pg->text[l-1] == '\n' || pg->text[l-1] == '\r' || pg->text[l-1] == ' ')) pg->text[--l] = 0;
    d->npages = 1; d->active = l > 0;
    return d->active;
}

#define DLG_OPEN_FRAMES 12      /* box grow/shrink (FUN_00475480 arg 0xc) */
#define DLG_CLOSE_FRAMES 22     /* FUN_0042bc60: DAT_00ac9bd8 = -22 */
#define DLG_CHARS_PER_FRAME (50.0f / DLG_OPEN_FRAMES)   /* FUN_00475610: 0x7ca0f4 / open frames */

void dialog_close(Dialog *d) { if (d->active && d->t > 0) { d->t = -DLG_CLOSE_FRAMES; d->box = -DLG_OPEN_FRAMES; d->closing = true; d->frame = DLG_CLOSE_FRAMES; } }

bool dialog_text_done(const Dialog *d) { return d->active && d->page == d->npages - 1 && d->done; }

/* FUN_0042d690 (state 0xd) + FUN_0042b250 + FUN_00476de0: START closes the page at once, an action button once the
 * text is complete; a held action button types 3x faster. Pages close over 22 frames and the next one opens. */
void dialog_update(Dialog *d, const Input *in, float dt)
{
    (void)dt;
    if (!d->active) return;
    if (d->t < 0) {
        d->t++; d->frame = -d->t;
        if (d->box < 0) d->box++;
        if (d->t == 0) {
            if (++d->page >= d->npages) { d->active = false; d->page = d->npages - 1; return; }
            d->t = 1; d->box = 0; d->chars = 0; d->done = false; d->closing = false; d->frame = 1;
        }
        return;
    }
    if (d->t == 0) { d->t = 1; d->frame = 1; }
    if (d->pending_sfx) { sfx_play_id(d->pending_sfx); d->pending_sfx = 0; }
    bool act = btn_pressed(in, BTN_SHOOT) || btn_pressed(in, BTN_JUMP);
    if (btn_pressed(in, BTN_PAUSE) || (d->done && act)) { dialog_close(d); sfx_play(0, 0); return; }
    d->t++; d->frame = d->t;
    if (d->box < DLG_OPEN_FRAMES) { d->box++; return; }
    if (d->done) return;
    bool held = btn_down(in, BTN_SHOOT) || btn_down(in, BTN_JUMP);
    d->chars += DLG_CHARS_PER_FRAME * (held ? 3 : 1);
    if (d->chars >= (float)strlen(d->pages[d->page].text)) { d->chars = (float)strlen(d->pages[d->page].text); d->done = true; }
}

/* word-wrapped text into lines of at most `maxw` pixels */
static int wrap(const Font *f, const char *text, int maxw, char lines[8][80])
{
    int n = 0, len = 0; lines[0][0] = 0;
    const char *s = text;
    while (*s && n < 8) {
        if (*s == '\n') { lines[n][len] = 0; n++; len = 0; if (n < 8) lines[n][0] = 0; s++; continue; }
        if (*s == ' ' && len == 0) { while (*s == ' ' && len < 78) lines[n][len++] = *s++; lines[n][len] = 0; continue; }   /* leading spaces are kept (scripts indent with them) */
        const char *w = s; while (*w && *w != ' ' && *w != '\n') w++;
        int wl = (int)(w - s);
        char word[80]; int wn = wl < 79 ? wl : 79; memcpy(word, s, wn); word[wn] = 0;
        char cand[80]; snprintf(cand, sizeof cand, "%s%s%s", lines[n], (len && lines[n][len - 1] != ' ') ? " " : "", word);
        if (len && font_text_width(f, cand) > maxw) { lines[n][len] = 0; n++; len = 0; if (n >= 8) break; lines[n][0] = 0; continue; }
        strcpy(lines[n], cand); len = (int)strlen(cand);
        s = w; while (*s == ' ') s++;
    }
    if (n < 8) { lines[n][len] = 0; n++; }
    return n;
}

/* FUN_00474610: 16 px corner tiles, edges and centre stretched; corners shrink when the box is smaller than two tiles */
static void draw_box(Sprite *ts, float x0, float y0, float x1, float y1)
{
    if (!ts || ts->frames < 9) return;
    float w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    float cw = (float)ts->w, ch = (float)ts->h;
    if (cw * 2 > w) cw = w * 0.5f;
    if (ch * 2 > h) ch = h * 0.5f;
    float mx = w - 2 * cw, my = h - 2 * ch;
    sprite_draw_scaled(ts, 0, x0, y0, cw, ch);
    if (mx > 0) sprite_draw_scaled(ts, 1, x0 + cw, y0, mx, ch);
    sprite_draw_scaled(ts, 2, x1 - cw, y0, cw, ch);
    if (my > 0) {
        sprite_draw_scaled(ts, 3, x0, y0 + ch, cw, my);
        if (mx > 0) sprite_draw_scaled(ts, 4, x0 + cw, y0 + ch, mx, my);
        sprite_draw_scaled(ts, 5, x1 - cw, y0 + ch, cw, my);
    }
    sprite_draw_scaled(ts, 6, x0, y1 - ch, cw, ch);
    if (mx > 0) sprite_draw_scaled(ts, 7, x0 + cw, y1 - ch, mx, ch);
    sprite_draw_scaled(ts, 8, x1 - cw, y1 - ch, cw, ch);
}

/* FUN_0042a5e0 / FUN_0042b250 geometry: box x (SW-226)/2..SW-(SW-274)/2 with an avatar, (SW-274)/2.. without,
 * y SH-64..SH-16; text at (+8,+4), 10 px lines; avatar 32x32 at (x0-32+6, y0-8); page marker = glyph 0x7f at
 * (x1-20, y1-10), blinking on bit 4 of the frame counter. */
void dialog_draw(const Dialog *d, SDL_Renderer *r, int sw, int sh)
{
    (void)r;
    if (!d->active || d->box == 0) return;
    const DialogPage *pg = &d->pages[d->page];
    Font *f = font_get(0x12072E60);
    Sprite *av = pg->avatar_id ? sprite_get(pg->avatar_id) : NULL;
    int x0 = av ? (sw - 226) / 2 : (sw - 274) / 2, x1 = sw - (sw - 274) / 2;
    int y0 = sh - 64, y1 = sh - 16;
    int k = d->box > 0 ? (d->box < DLG_OPEN_FRAMES ? d->box : DLG_OPEN_FRAMES) : -d->box;
    float s = (float)k / DLG_OPEN_FRAMES;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hw = (x1 - x0) * 0.5f * s, hh = (y1 - y0) * 0.5f * s;
    draw_box(sprite_get(TILESET[pg->color]), floorf(cx - hw), floorf(cy - hh), floorf(cx + hw), floorf(cy + hh));
    if (av) sprite_draw(av, 0, (float)(x0 - av->w + 6), (float)(y0 - 8), false);
    if (d->box < DLG_OPEN_FRAMES || !f) return;
    char lines[8][80]; int n = wrap(f, pg->text, x1 - x0 - 16, lines);
    int remaining = (int)d->chars, ly = y0 + 4;
    for (int i = 0; i < n && remaining > 0; i++) {
        int l = (int)strlen(lines[i]);
        font_draw_n(f, lines[i], remaining < l ? remaining : l, (float)(x0 + 8), (float)(ly + i * 10), TEXTRGB[pg->color][0], TEXTRGB[pg->color][1], TEXTRGB[pg->color][2]);
        remaining -= l + 1;
    }
    if (d->done && (d->t & 16)) { char m[2] = { 0x7f, 0 }; font_draw(f, m, (float)(x1 - 20), (float)(y1 - 10), TEXTRGB[pg->color][0], TEXTRGB[pg->color][1], TEXTRGB[pg->color][2]); }
}
