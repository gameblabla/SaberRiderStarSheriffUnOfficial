#include "menu.h"
#include "gfx.h"
#include "pack.h"
#include "audio.h"
#include "font.h"
#include "dialog.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static void fill(SDL_Renderer *r, int sw, int sh, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_FRect q = { 0, 0, (float)sw, (float)sh }; SDL_RenderFillRect(r, &q);
}
static bool confirm(const Input *in) { return btn_pressed(in, BTN_PAUSE) || btn_pressed(in, BTN_JUMP) || btn_pressed(in, BTN_SHOOT); }

void menu_enter(Menu *m, int state)
{
    if (m->video) { video_close(m->video); m->video = NULL; }
    m->state = state; m->t = 0; m->text_chars = 0;
    switch (state) {
    case MS_SPLASH0: case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3: m->dur = 3.0f; break;
    case MS_INTRO: m->dur = 0; break;
    case MS_MAIN: m->dur = 3.0f; m->sel = 0; music_play(0, true); break;
    case MS_CHARSEL: m->dur = 1.0f; m->character = 1; music_play(1, true); break;
    case MS_BRIEFING: m->dur = 1.0f; music_stop(); break;
    case MS_GAMEOVER: m->dur = 6.0f; music_play(7, false); break;
    case MS_ACCOMPLISHED: m->dur = 6.0f; music_play(6, false); break;
    default: m->dur = 1.0f; break;
    }
}

void menu_update(Menu *m, const Input *in, float dt, int sw, SDL_Renderer *r)
{
    (void)sw;
    m->t += dt;
    switch (m->state) {
    case MS_SPLASH0: case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3:
        if (m->t >= m->dur || confirm(in)) menu_enter(m, m->state + 1);
        break;
    case MS_INTRO:
        if (!m->video) { m->video = video_open(r, 0xE46721E5); if (!m->video) { menu_enter(m, MS_MAIN); break; } }
        if (!video_update(m->video, dt) || confirm(in)) menu_enter(m, MS_MAIN);
        break;
    case MS_MAIN:
        if (m->t < m->dur) { if (confirm(in)) m->t = m->dur; break; }
        if (btn_pressed(in, BTN_UP) || btn_pressed(in, BTN_DOWN)) { m->sel ^= 1; sfx_play(8, 0); }
        if (confirm(in)) { sfx_play(9, 0); menu_enter(m, m->sel == 0 ? MS_CHARSEL : MS_OPTIONS); }
        break;
    case MS_OPTIONS:
        if (confirm(in)) menu_enter(m, MS_MAIN);
        break;
    case MS_CHARSEL:
        if (btn_pressed(in, BTN_LEFT)) { m->character = (m->character + 2) % 3; sfx_play(8, 0); }
        if (btn_pressed(in, BTN_RIGHT)) { m->character = (m->character + 1) % 3; sfx_play(8, 0); }
        if (confirm(in) && m->t > 0.3f) { if (m->character == 1) { sfx_play(9, 0); menu_enter(m, MS_BRIEFING); } else sfx_play(11, 0); }   /* only Fireball is playable in the demo */
        break;
    case MS_BRIEFING:
        if (!m->video) m->video = video_open(r, 0x2FE798C3);
        m->text_chars += dt * 30.0f;
        if (m->video && !video_update(m->video, dt)) { video_close(m->video); m->video = NULL; }
        if (confirm(in) && m->t > 0.5f) { if (m->video) { video_close(m->video); m->video = NULL; } m->start_level = true; }
        else if (!m->video && m->t > 8.0f) m->start_level = true;
        break;
    case MS_GAMEOVER: case MS_ACCOMPLISHED:
        if (m->t >= m->dur || (m->t > 1.5f && confirm(in))) menu_enter(m, MS_MAIN);
        break;
    default: break;
    }
}

static void draw_centered(Sprite *s, int sw, int y) { if (s) sprite_draw(s, 0, (float)((sw - s->w) / 2), (float)y, false); }

void menu_draw(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    float fade = 1.0f;   /* 1 = fully visible; splash screens fade in/out with a sine */
    if (m->state <= MS_SPLASH3 || m->state == MS_GAMEOVER || m->state == MS_ACCOMPLISHED) {
        float x = sinf(3.1415927f * (m->t / m->dur)) * 2.0f; fade = x > 1 ? 1 : x;
    }
    switch (m->state) {
    case MS_SPLASH0: { fill(r, sw, sh, 0, 0, 0, 255); Sprite *s = sprite_get(0x7C5F519A); draw_centered(s, sw, s ? (sh - s->h) / 2 : 0); break; }
    case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3: {
        fill(r, sw, sh, 255, 255, 255, 255);
        static const uint32_t ids[3] = { 0x7C655085, 0x7C6B538C, 0x7C71528B };
        Sprite *s = sprite_get(ids[m->state - 1]);
        if (s) {
            float sc = m->state == MS_SPLASH3 && m->t >= 2.0f ? 1.0f : 0.76f + 0.24f * fade;   /* zoom-in (0x7c422c/0x7c5854) */
            sprite_draw_scaled(s, 0, (sw - s->w * sc) * 0.5f, (sh - s->h * sc) * 0.5f, s->w * sc, s->h * sc);
        }
        break; }
    case MS_INTRO: fill(r, sw, sh, 0, 0, 0, 255); video_draw(m->video, r, sw, sh); return;
    case MS_MAIN: case MS_OPTIONS: {
        Sprite *bg = sprite_get(0xD7DEBAC0), *logo = sprite_get(0xB04BAC5F), *shadow = sprite_get(0x989121EC);
        Sprite *start = sprite_get(0x01E9B701), *opt = sprite_get(0x8FF0AB30);
        float open = m->t < m->dur ? m->t / m->dur : 1.0f;
        if (bg) { sprite_draw(bg, 0, 0, 0, false); sprite_draw(bg, 0, (float)(sw - bg->w), 0, true); }
        if (logo) {
            int lx = (sw - logo->w) / 2, ly = 0x20 - (int)((1 - open) * 120);
            if (shadow) { SDL_SetTextureAlphaMod(shadow->tex, 80); sprite_draw(shadow, 0, (float)lx, (float)ly, false); SDL_SetTextureAlphaMod(shadow->tex, 255); }
            sprite_draw(logo, 0, (float)lx, (float)ly, false);
        }
        if (open >= 1.0f) {
            bool blink = ((SDL_GetTicks() / 16) & 0x1f) < 8;
            if (start) { if (m->sel == 0 && !blink) SDL_SetTextureColorMod(start->tex, 255, 255, 0); draw_centered(start, sw, 0xc0); SDL_SetTextureColorMod(start->tex, 255, 255, 255); }
            if (opt) { if (m->sel == 1 && !blink) SDL_SetTextureColorMod(opt->tex, 255, 255, 0); draw_centered(opt, sw, 0xd0); SDL_SetTextureColorMod(opt->tex, 255, 255, 255); }
            if (m->state == MS_OPTIONS) { Font *f = font_get(0x12072E60); fill(r, sw, sh, 0, 0, 0, 160); if (f) font_draw(f, "OPTIONS NOT IMPLEMENTED YET", 100, 110, 255, 255, 255); }
        } else fade = open;
        break; }
    case MS_CHARSEL: {
        Sprite *bg = sprite_get(0x92702CF3), *moon = sprite_get(0x2178AD91), *title = sprite_get(0x4813ED48);
        Sprite *prev = sprite_get(0xAE16B01D), *next = sprite_get(0xE34D3083);
        /* panels: Saber (gold), Fireball (red), April (green); selected shows the portrait + white name */
        static const uint32_t FRAME[3] = { 0xC2EBBACE, 0x13D53116, 0xCDC8A9CC };
        static const uint32_t PORT[3]  = { 0x67C9A3D9, 0x74100546, 0x72A6B0FB };
        static const uint32_t NAME_ON[3]  = { 0xB48828F4, 0x699DC4C3, 0xA9AB3BF0 };
        static const uint32_t NAME_OFF[3] = { 0xEC2B5E94, 0x2F75D0AA, 0xA9AB3BF0 };
        fill(r, sw, sh, 0, 0, 0, 255);
        if (bg) { float ox = fmodf(m->t * 8.0f, (float)bg->w); sprite_draw(bg, 0, -ox, (float)((sh - bg->h) / 2), false); sprite_draw(bg, 0, bg->w - ox, (float)((sh - bg->h) / 2), false); }
        if (moon) sprite_draw(moon, 0, (float)((sw - moon->w) / 2), (float)((sh - moon->h) / 2), false);
        int x0 = (sw - 0x120 - 16) / 2;
        if (title) sprite_draw(title, 0, (float)(x0 + 0x20), 0x10, false);
        if (prev) sprite_draw(prev, 0, (float)x0, 0x60, false);
        if (next) sprite_draw(next, 0, (float)(x0 + 0x120), 0x60, false);
        int px = (sw - (80 + 8 + 64 + 8 + 64)) / 2;
        for (int i = 0; i < 3; i++) {
            Sprite *fr = sprite_get(FRAME[i]);
            bool on = i == m->character;
            if (fr) { if (!on) SDL_SetTextureAlphaMod(fr->tex, 96); sprite_draw(fr, 0, (float)px, 0x30, false); SDL_SetTextureAlphaMod(fr->tex, 255); }
            if (on) { Sprite *po = sprite_get(PORT[i]); if (po) sprite_draw(po, 0, (float)px, 0x30, false); }
            Sprite *nm = sprite_get(on ? NAME_ON[i] : NAME_OFF[i]);
            if (nm) sprite_draw(nm, 0, (float)(px + ((fr ? fr->w : 64) - nm->w) / 2), 0x30 + 160 + 4, false);
            px += (fr ? fr->w : 64) + 8;
        }
        break; }
    case MS_BRIEFING: {
        Sprite *room = sprite_get(0x0EAE8AEB);
        fill(r, sw, sh, 0, 0, 0, 255);
        if (room) sprite_draw(room, 0, (float)((sw - room->w) / 2), 0, false);
        if (m->video && m->t > 1.0f) {
            /* the briefing video plays on the big screen in the upper part of the room art (768x312 -> 234x95) */
            SDL_Rect clip = { 104, 60, 218, 88 };
            SDL_SetRenderClipRect(r, &clip);
            video_draw(m->video, r, sw, 60 * 2 + 88);
            SDL_SetRenderClipRect(r, NULL);
        }
        const PackEntry *e = packs_find(0x29CAD5D3);
        Font *f = font_get(0x12072E60);
        if (e && f && m->t > 1.5f) {
            char buf[256]; size_t n = e->size < 255 ? e->size : 255; memcpy(buf, e->data, n); buf[n] = 0;
            char *text = strchr(buf, '\n'); text = text ? text + 1 : buf;   /* first line = video name */
            int shown = (int)m->text_chars, y = 196;
            char *line = strtok(text, "\n");
            while (line && shown > 0) { int l = (int)strlen(line); font_draw_n(f, line, shown < l ? shown : l, (float)((sw - font_text_width(f, line)) / 2), (float)y, 255, 230, 120); shown -= l; y += 12; line = strtok(NULL, "\n"); }
        }
        break; }
    case MS_GAMEOVER: { fill(r, sw, sh, 0, 0, 0, 255); Sprite *s = sprite_get(0xF629241D); draw_centered(s, sw, s ? (sh - s->h) / 2 : 0); break; }
    case MS_ACCOMPLISHED: {
        fill(r, sw, sh, 0, 0, 0, 255);
        Sprite *a = sprite_get(0x24138418), *b = sprite_get(0xF8F9017C);
        draw_centered(a, sw, sh / 2 - 34); draw_centered(b, sw, sh / 2 - 4);
        break; }
    default: break;
    }
    if (fade < 1.0f) fill(r, sw, sh, 0, 0, 0, (uint8_t)((1.0f - fade) * 255));
}
