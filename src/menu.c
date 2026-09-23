#include "menu.h"
#include "heroes.h"
#include "gfx.h"
#include "pack.h"
#include "audio.h"
#include "font.h"
#include "assets.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.1415927f
#define STEP (1.0f / 60.0f)
#define MENU_PERIOD 2.4f          /* +0x60: splash / zoom period */
#define ATTRACT_FRAMES 1800       /* title idle -> intro (DAT_007c5bac) */

static void fill(SDL_Renderer *r, int sw, int sh, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    if (A == 0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_FRect q = { 0, 0, (float)sw, (float)sh }; SDL_RenderFillRect(r, &q);
}
static bool confirm(const Input *in) { return btn_pressed(in, BTN_PAUSE) || btn_pressed(in, BTN_JUMP) || btn_pressed(in, BTN_SHOOT); }
static bool action(const Input *in) { return btn_pressed(in, BTN_JUMP) || btn_pressed(in, BTN_SHOOT); }
static uint8_t clamp255(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v; }
/* the menu's universal easing: min(1, 2 sin(pi t / period)) — 0 at both ends of the period, 1 in the middle */
static float ease(float t, float dur) { float x = 2.0f * sinf(PI * t / dur); return x > 1 ? 1 : x < 0 ? 0 : x; }
static float zoom_scale(float f) { return 32.0f - 31.0f * f; }   /* DAT_007c422c + DAT_007c5854 * f */
static bool blink_on(int mask, int lim) { return ((int)(SDL_GetTicks() / 16) & mask) < lim; }
static uint32_t frame_no(void) { return (uint32_t)(SDL_GetTicks() / 16); }
static void draw_centered(Sprite *s, int sw, int y) { if (s) sprite_draw(s, 0, (float)((sw - s->w) / 2), (float)y, false); }
/* triangle-wave alpha used by the result screens: (frame*2)&0x7f folded into 0x33..0x73, relative to a 0x80 neutral */
static uint8_t pulse_alpha(void) { unsigned u = (frame_no() * 2) & 0x7f; unsigned a = u < 0x40 ? u + 0x33 : 0xb3 - u; return (uint8_t)(a * 2); }
/* highlighted menu item colour (0x80805b00 -> (255,182,0)), white while blinking */
static void hilite(int on, uint8_t *R, uint8_t *G, uint8_t *B) { if (on && !blink_on(0x1f, 8)) { *R = 255; *G = 182; *B = 0; } else { *R = *G = *B = 255; } }

static void lives_caps(int difficulty, int *max_lives, int *max_cont)
{
    *max_lives = difficulty == 0 ? 7 : difficulty == 1 ? 5 : 3;
    *max_cont  = difficulty == 0 ? 5 : difficulty == 1 ? 4 : 3;
}

static void open_briefing(Menu *m, SDL_Renderer *r)
{
    const PackEntry *e = packs_find(0x29CAD5D3);   /* briefing script: line 1 = video name, rest = text */
    if (!e) return;
    char buf[512]; size_t n = e->size < sizeof buf - 1 ? e->size : sizeof buf - 1; memcpy(buf, e->data, n); buf[n] = 0;
    char *text = strchr(buf, '\n'); if (text) *text++ = 0; else text = buf;
    (void)r;
    dialog_open_text(&m->dlg, text, DLG_GREEN);
    m->video = video_open(r, 0x2FE798C3);
}

void menu_enter(Menu *m, int state)
{
    if (m->video) { video_close(m->video); m->video = NULL; }
    int prev = m->state;
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "menu %d -> %d\n", prev, state);
    m->state = state; m->t = 0; m->dur = MENU_PERIOD; m->idle_frames = 0;
    switch (state) {
    case MS_SPLASH0: case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3: case MS_INTRO:
        music_stop(); if (state == MS_INTRO) m->dur = 0; break;   /* FUN_00411480 when a result screen / the title hands over */
    case MS_MAIN:
        if (prev == MS_OPTIONS || prev == MS_CREDITS) m->t = m->dur * 0.5f;   /* no zoom-in when coming back from a sub menu */
        m->sel = 0; music_play(0, true); break;
    case MS_OPTIONS: m->sel = 1; m->music_track = 0; if (prev != MS_CREDITS) music_play(3, true); break;
    case MS_BRIEFING: m->dlg.active = false; music_play(2, true); break;
    case MS_CHARSEL: m->t = -0.25f; m->character = 1; music_play(1, true); break;
    case MS_GAMEOVER: m->dur = 3.0f; music_play(4, false); break;        /* FUN_0042d690 -> state 9 + music 4 */
    case MS_CONTINUE: music_stop(); m->continue_now = false; break;      /* silence but the clock ticks */
    case MS_ACCOMPLISHED: m->dur = 10.0f; music_play(7, false); break;   /* state 0xf + music 7 */
    case MS_CREDITS: m->credits_page = 0; m->credits_t = 0; music_play(9, true); break;   /* FUN_004265a0: backer credits music */
    default: break;
    }
}

void menu_update(Menu *m, const Input *in, float dt, int sw, SDL_Renderer *r)
{
    (void)sw;
    switch (m->state) {
    case MS_SPLASH0: case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3: {
        /* FUN_00425ee0: the "please note" splash runs at 1/3 speed between t=1 and t=2; the last one runs double after t=2 */
        float step = dt;
        if (m->state == MS_SPLASH1 && m->t > 1.0f && m->t < 2.0f) step = dt / 3.0f;
        if (m->state == MS_SPLASH3 && m->t > 2.0f) step = dt * 2.0f;
        m->t += step;
        if (btn_pressed(in, BTN_PAUSE)) { menu_enter(m, MS_MAIN); break; }
        if (m->t >= m->dur || action(in)) menu_enter(m, m->state + 1);
        break; }
    case MS_INTRO:
        if (!m->video) { m->video = video_open(r, 0xE46721E5); if (!m->video) { menu_enter(m, MS_MAIN); break; } }
        if (!video_update(m->video, dt) || confirm(in)) menu_enter(m, MS_MAIN);
        break;
    case MS_MAIN: {
        float f = ease(m->t, m->dur);
        if (f < 1.0f && m->t < m->dur * 0.5f) { m->t += dt; break; }   /* zoom-in */
        bool any = false;
        for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        if (btn_pressed(in, BTN_DOWN) && m->sel == 0) { m->sel = 1; sfx_play(0, 0); }
        else if (btn_pressed(in, BTN_UP) && m->sel > 0) { m->sel--; sfx_play(0, 0); }
        if (confirm(in)) {
            sfx_play(0, 0);
            if (m->sel == 1) menu_enter(m, MS_OPTIONS);
            else { menu_enter(m, MS_BRIEFING); open_briefing(m, r); }
            break;
        }
        m->idle_frames = any ? 0 : m->idle_frames + 1;
        if (m->idle_frames > ATTRACT_FRAMES) {   /* attract: fade out and replay the intro */
            m->t += dt;
            if (m->t >= m->dur * 0.5f + 1.0f) menu_enter(m, MS_INTRO);
        } else m->t = m->dur * 0.5f;
        break; }
    case MS_OPTIONS: {
        int maxl, maxc;
        if (btn_pressed(in, BTN_UP)) { m->sel = m->sel == 0 ? OPT_COUNT - 1 : m->sel - 1; sfx_play(0, 0); }
        if (btn_pressed(in, BTN_DOWN)) { m->sel = m->sel == OPT_COUNT - 1 ? 0 : m->sel + 1; sfx_play(0, 0); }
        int dir = btn_pressed(in, BTN_RIGHT) ? 1 : btn_pressed(in, BTN_LEFT) ? -1 : 0;
        if (dir) sfx_play(0, 0);
        switch (m->sel) {
        case OPT_LEVEL:
            if (dir) { m->difficulty = (m->difficulty + 3 + dir) % 3; lives_caps(m->difficulty, &maxl, &maxc); if (m->lives > maxl) m->lives = maxl; if (m->continues > maxc) m->continues = maxc; }
            break;
        case OPT_PLAYER: lives_caps(m->difficulty, &maxl, &maxc); m->lives += dir; if (m->lives < 0) m->lives = 0; if (m->lives > maxl) m->lives = maxl; break;
        case OPT_CONTINUE: lives_caps(m->difficulty, &maxl, &maxc); m->continues += dir; if (m->continues < 0) m->continues = 0; if (m->continues > maxc) m->continues = maxc; break;
        case OPT_SCREEN: if (dir) { m->screen = (m->screen + 4 + dir) % 4; m->apply_screen_mode = true; } break;
        case OPT_RATIO:   /* WIDE -> 4:3 -> STRETCH -> WIDE */
            if (dir) { m->ratio = m->ratio == RATIO_WIDE ? (dir > 0 ? RATIO_43 : RATIO_STRETCH) : m->ratio == RATIO_43 ? (dir > 0 ? RATIO_STRETCH : RATIO_WIDE) : (dir > 0 ? RATIO_WIDE : RATIO_43); m->apply_screen_mode = true; }
            break;
        case OPT_FILTER: if (dir) m->filter = (m->filter + FILTER_COUNT + dir) % FILTER_COUNT; break;
        case OPT_MUSIC:
            if (dir || confirm(in)) {
                m->music_track = (m->music_track + 9 + (dir ? dir : 1)) % 9;
                music_play(m->music_track == 0 ? 3 : 9 + m->music_track, true);   /* tracks 1..8 -> music table 10..17 */
            }
            break;
        default: break;
        }
        if (confirm(in)) {
            if (m->sel == OPT_EXIT) { sfx_play(0, 0); menu_enter(m, MS_MAIN); }
            else if (m->sel == OPT_CREDITS) { sfx_play(0, 0); menu_enter(m, MS_CREDITS); }
        }
        break; }
    case MS_CREDITS: {
        const PackEntry *e = packs_find(0x7E11BC19);
        int pages = 1; if (e) for (uint32_t i = 0; i + 6 <= e->size; i++) if (!memcmp(e->data + i, "[fade]", 6)) pages++;
        m->credits_t += dt;
        if (m->credits_t >= 3.5f) { m->credits_t = 0; m->credits_page++; }
        if (m->credits_page >= pages || confirm(in)) {
            if (m->ending) { m->ending = false; menu_enter(m, MS_SPLASH1); }   /* the game's end: back round to the title */
            else menu_enter(m, MS_OPTIONS);
        }
        break; }
    case MS_BRIEFING: {
        /* FUN_00429050: text plays; START (or an action button once the text is complete) closes the box, then the
         * four hero pieces slide in (t 0..0.25), hold, fade to black (1.75..2) and character select follows */
        if (m->video && !video_update(m->video, dt)) { /* keep the last frame on the screen */ }
        if (m->t == 0.0f) {
            if (m->dlg.active) dialog_update(&m->dlg, in, dt);
            else { if (m->video) { video_close(m->video); m->video = NULL; } m->t = 0.0001f; sfx_play(8, 0); }
        } else {
            m->t += dt;
            if (m->t > 2.0f) menu_enter(m, MS_CHARSEL);
        }
        break; }
    case MS_CHARSEL:
        if (m->t < 0.0f) { m->t += dt; if (m->t > 0) m->t = 0; break; }   /* fade in */
        if (m->t == 0.0f) {
            if (btn_pressed(in, BTN_LEFT) && m->character > 0) { m->character--; sfx_play(0, 0); }
            if (btn_pressed(in, BTN_RIGHT) && m->character < 3) { m->character++; sfx_play(0, 0); }
            if (confirm(in)) {
                if (!hero_available(m->character)) sfx_play(23, 0);   /* the demo only had Fireball; April is our reconstruction */
                else { hero_select_sfx(m->character); m->t = STEP; }
            }
        } else {
            m->t += dt;
            music_set_volume(1.0f - m->t);   /* FUN_00429510: the select music fades out over the 1 s LOADING */
            if (m->t >= 1.0f) { m->start_level = true; m->t = 1.0f; }
        }
        break;
    case MS_GAMEOVER:   /* FUN_0042a210: zoom/fade in for 1 s, hold for START or an action button, then ~1.9 s out */
        if (m->t < 1.0f || m->t > 1.0166667f || confirm(in)) m->t += dt;
        if (m->t > 2.0f) music_set_volume(3.0f - m->t);   /* FUN_00425e70(3 - t) while fading out */
        if (m->t > 2.9f) menu_enter(m, MS_SPLASH1);
        break;
    case MS_CONTINUE: {   /* the count runs 20 -> 0, one per second; START / an action button takes the continue, 0 = GAME OVER */
        float prev = m->t; m->t += dt;
        if (m->t < 0.5f) break;   /* the screen fades in first */
        int before = CONTINUE_FROM - (int)(prev - 0.5f), now = CONTINUE_FROM - (int)(m->t - 0.5f);
        if (now != before && now >= 0) sfx_play(0, 0);
        if (now < 0) { menu_enter(m, MS_GAMEOVER); break; }
        if (confirm(in)) { sfx_play(8, 0); m->continue_now = true; }
        break; }
    case MS_ACCOMPLISHED:   /* FUN_00429de0: 4 s, then a 1 s countdown to the splash (or the next stage) */
        m->t += dt;
        if (m->t > 5.0f) { if (m->more_stages) { m->more_stages = false; m->next_stage = true; music_stop(); } else menu_enter(m, m->ending ? MS_CREDITS : MS_SPLASH1); }
        break;
    default: break;
    }
}

/* ---- drawing ---- */

static void draw_title_bg(Sprite *bg, int sw, int sh, float s)
{
    /* the title background is a half-screen strip drawn twice, the right copy mirrored */
    if (!bg) return;
    float w = bg->w * s, h = bg->h * s, cx = sw * 0.5f, y = (sh - h) * 0.5f;
    if (s == 1.0f) { sprite_draw(bg, 0, 0, 0, false); sprite_draw(bg, 0, (float)(sw - bg->w), 0, true); return; }
    SDL_FRect src = { 0, 0, (float)bg->w, (float)bg->h };
    SDL_FRect l = { cx - w, y, w, h }, rr = { cx, y, w, h };
    SDL_RenderTexture(SDL_GetRendererFromTexture(bg->tex), bg->tex, &src, &l);
    SDL_RenderTextureRotated(SDL_GetRendererFromTexture(bg->tex), bg->tex, &src, &rr, 0, NULL, SDL_FLIP_HORIZONTAL);
}

static void draw_spiral(Menu *m, int sw, int sh, uint8_t bright)
{
    /* rotating spiral + counter-rotating moon (character select / options backdrop) */
    Sprite *sp = sprite_get(0x92702CF3), *moon = sprite_get(0x2178AD91);
    float deg = m->angle * 180.0f / PI;
    if (sp) sprite_draw_rotated(sp, 0, sw * 0.5f, sh * 0.5f, sw / 420.0f, -deg, bright, 255);
    if (moon) sprite_draw_rotated(moon, 0, sw * 0.5f, sh * 0.5f, 1.0f, deg, bright, 255);
}

static void draw_options(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    m->angle += STEP;
    draw_spiral(m, sw, sh, 0x33 * 2);
    Sprite *holes = sprite_get(0xE740F153), *hdr = sprite_get(0xF8F9017C), *ex = sprite_get(0x94C9A3DA);
    if (holes) {
        if (m->ratio == RATIO_43) sprite_draw(holes, 0, (float)((sw - holes->w) / 2), (float)((sh - holes->h) / 2), false);
        else sprite_draw_scaled(holes, 0, 0, 0, (float)sw, (float)sh);
    }
    draw_centered(hdr, sw, 0x10);
    uint8_t R, G, B;
    if (ex) { hilite(m->sel == OPT_EXIT, &R, &G, &B); sprite_draw_mod(ex, 0, (float)((sw - ex->w) / 2), 0xd0, R, G, B, 255); }
    Font *f = font_get(0x4058897F);
    if (!f) return;
    int x0 = sw / 2 - 256, y0 = sh / 2 - 256;   /* the spiral's top-left corner is the layout origin */
    int lx = x0 + 0xb0, vx = x0 + 0x11c;
    static const char *DIFF[3] = { "EASY", "NORMAL", "HARD" };
    static const char *FILT[FILTER_COUNT] = { "NONE", "CRT", "DOUBLE", "DOUBLE+SCANLINES", "CRT+SCANLINES" };
    char lives[16], cont[16], scr[32], mus[24];
    snprintf(lives, sizeof lives, "%02d", m->lives); snprintf(cont, sizeof cont, "%02d", m->continues);
    if (m->screen == 0) snprintf(scr, sizeof scr, "FULL %ux%u", 852, 480); else snprintf(scr, sizeof scr, "WINDOWED x%u", m->screen + 1);
    if (m->music_track == 0) snprintf(mus, sizeof mus, "OPTIONS"); else snprintf(mus, sizeof mus, "TEST TRACK%02d", m->music_track);
    const char *rows[7][2] = { { "LEVEL", DIFF[m->difficulty] }, { "PLAYER", lives }, { "CONTINUE", cont }, { "SCREEN", scr },
                               { "RATIO", m->ratio == RATIO_WIDE ? "WIDE" : m->ratio == RATIO_43 ? "4:3" : "STRETCH" }, { "FILTER", FILT[m->filter] }, { "MUSIC TEST", mus } };
    for (int i = 0; i < 7; i++) {
        hilite(m->sel == OPT_LEVEL + i, &R, &G, &B);
        font_draw(f, rows[i][0], (float)lx, (float)(y0 + 0xd8 + i * 0x10), R, G, B);
        font_draw(f, rows[i][1], (float)vx, (float)(y0 + 0xd8 + i * 0x10), 255, 255, 255);
    }
    hilite(m->sel == OPT_CREDITS, &R, &G, &B);
    font_draw(f, "BACKER CREDITS", (float)(x0 + 0xca), (float)(y0 + 0x14e), R, G, B);
    (void)r;
}

static void draw_main(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    Sprite *bg = sprite_get(0xD7DEBAC0), *logo = sprite_get(0xB04BAC5F), *shadow = sprite_get(0x989121EC);
    Sprite *start = sprite_get(0x01E9B701), *opt = sprite_get(0x8FF0AB30);
    float f = ease(m->t, m->dur);
    if (f < 1.0f && m->t < m->dur * 0.5f) {   /* zoom-in from 32x, fading up from black (FUN_00428b30) */
        float s = zoom_scale(f);
        draw_title_bg(bg, sw, sh, s);
        if (logo) sprite_draw_scaled(logo, 0, (sw - logo->w * s) * 0.5f, (sh - 16 * s - logo->h * s) * 0.5f, logo->w * s, logo->h * s);
        fill(r, sw, sh, 0, 0, 0, clamp255((1 - f) * 255));
        return;
    }
    draw_title_bg(bg, sw, sh, 1.0f);
    if (logo) {
        int lx = (sw - logo->w) / 2;
        if (shadow) sprite_draw_mod(shadow, 0, (float)lx, 0x20, 255, 255, 255, 80);
        sprite_draw(logo, 0, (float)lx, 0x20, false);
    }
    uint8_t R, G, B;
    if (start) { hilite(m->sel == 0, &R, &G, &B); sprite_draw_mod(start, 0, (float)((sw - start->w) / 2), 0xc0, R, G, B, 255); }
    if (opt) { hilite(m->sel == 1, &R, &G, &B); sprite_draw_mod(opt, 0, (float)((sw - opt->w) / 2), 0xd0, R, G, B, 255); }
    if (m->idle_frames > ATTRACT_FRAMES) fill(r, sw, sh, 0, 0, 0, clamp255((m->t - m->dur * 0.5f) * 255));
}

static void draw_briefing(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    Sprite *room = sprite_get(0x0EAE8AEB);
    fill(r, sw, sh, 0, 0, 0, 255);
    if (room) sprite_draw(room, 0, (float)((sw - room->w) / 2), (float)((sh - room->h) / 2), false);
    if (m->video) {
        /* FUN_0042ba20: the video plays at 1/3 scale on the room's big screen, bottom edge at sh-74; while the text box opens
         * (frames 1..21) it unfolds vertically from its centre line */
        int vw, vh; video_size(m->video, &vw, &vh);
        float w = vw / 3.0f, h = vh / 3.0f, x = (sw - w) * 0.5f, cy = sh - 74 - h * 0.5f;
        float half = h * 0.5f;
        if (m->dlg.active && !m->dlg.closing && m->dlg.frame < 21) half = m->dlg.frame * 0.0075757f * vh;
        if (m->dlg.closing) half = m->dlg.frame * 0.0075757f * vh;
        if (half > 0) {
            SDL_Rect clip = { (int)x, (int)(cy - half), (int)w + 1, (int)(half * 2) + 1 };
            SDL_SetRenderClipRect(r, &clip);
            video_draw_rect(m->video, r, x, cy - h * 0.5f, w, h);
            SDL_SetRenderClipRect(r, NULL);
        }
    }
    if (m->dlg.active) dialog_draw(&m->dlg, r, sw, sh);
    if (m->t > 0.0f) {   /* FUN_00429200: the four hero pieces slide in from the edges */
        CBlock *pc = cblock_get(0x2DEF1664);
        if (pc) {
            float pw = (float)(pc->cols * pc->tw), ph = (float)(pc->rows * pc->th);
            float x0 = floorf((sw - pw) * 0.5f), y0 = floorf((sh - ph) * 0.5f);
            float tt = m->t > 0.25f ? 0.25f : m->t;
            float f = sinf(tt * 2.0f * PI) * 0.25f;
            float dx = floorf(0.5f * (pw - f * 4.0f * pw)), dy = floorf(0.5f * (ph - f * 4.0f * ph));
            cblock_draw_frame(pc, 0, x0 - dx, y0, false);
            cblock_draw_frame(pc, 1, x0 + dx, y0, false);
            cblock_draw_frame(pc, 2, x0, y0 - dy, false);
            cblock_draw_frame(pc, 3, x0, y0 + dy, false);
        }
        if (m->t > 1.75f) fill(r, sw, sh, 0, 0, 0, clamp255((m->t - 1.75f) * 4.0f * 255));
    }
}

static void draw_charsel(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    /* FUN_004296d0 */
    m->angle += STEP;
    draw_spiral(m, sw, sh, 255);
    int x0 = (sw - 0x140) / 2;
    Sprite *title = sprite_get(0x4813ED48), *prev = sprite_get(0xAE16B01D), *next = sprite_get(0xE34D3083);
    Sprite *cursor = sprite_get(0x5B550481), *hl = sprite_get(0x7673D08E), *na = sprite_get(0x7255866F);
    if (title) sprite_draw(title, 0, (float)(x0 + 0x20), 0x10, false);
    bool arrows = (frame_no() & 0x20) != 0;
    if (prev && arrows) sprite_draw(prev, 0, (float)x0, 0x60, false);
    if (next && arrows) sprite_draw(next, 0, (float)(x0 + 0x120), 0x60, false);
    /* per panel: frame, name on/off, portrait on/off */
    static const uint32_t FRAME[4]    = { 0xC2EBBACE, 0x13D53116, 0xCDC8A9CC, 0x957325FD };
    static const uint32_t NAME_ON[4]  = { 0xB48828F4, 0x699DC4C3, 0xA9AB3BF0, 0xF6CBB2F4 };
    static const uint32_t NAME_OFF[4] = { 0xEC2B5E94, 0x2F75D0AA, 0xE14E4D96, 0xBFFFBB29 };
    static const uint32_t PORT_ON[4]  = { 0x67C9A3D9, 0x74100546, 0x72A6B0FB, 0xEEE2331F };
    static const uint32_t PORT_OFF[4] = { 0x3459994C, 0xAA051172, 0x3F368A4E, 0x217B03F1 };
    static const int PX[4] = { 0x10, 0x60, 0xa0, 0xe0 }, NA_ADJ[4] = { 0, 0, 8, 4 }, HL_ADJ[4] = { 0, 0xc, 8, 4 }, CUR_ADJ[4] = { 0xe, 2, 7, 10 };
    for (int i = 0; i < 4; i++) {
        bool on = i == m->character;
        int x = x0 + PX[i];
        Sprite *fr = sprite_get(FRAME[i]), *nm = sprite_get(on ? NAME_ON[i] : NAME_OFF[i]), *po = sprite_get(on ? PORT_ON[i] : PORT_OFF[i]);
        if (fr) sprite_draw_mod(fr, 0, (float)x, 0x30, 255, 255, 255, on ? 255 : 128);
        if (nm) sprite_draw(nm, 0, (float)(x + ((i == 0 || i == 3) ? 0x10 : 0)), 0xd0, false);
        if (po) sprite_draw(po, 0, (float)x, 0x30, false);
        if (!hero_available(i) && na) sprite_draw_mod(na, 0, (float)(x - NA_ADJ[i] + 8), 0x68, 255, 255, 255, 116);
        if (on && hl && (frame_no() & 8)) sprite_draw(hl, 0, (float)(x - HL_ADJ[i]), 0x30, false);
        if (on && cursor) sprite_draw(cursor, 0, (float)(x + CUR_ADJ[i]), 0x22, false);
    }
    if (m->t < 0.0f) fill(r, sw, sh, 0, 0, 0, clamp255(-m->t * 4.0f * 255));
    else if (m->t > 0.0f) {
        fill(r, sw, sh, 0, 0, 0, clamp255(ease(m->t, m->dur) * 255));
        Font *f = font_get(0x4058897F);
        if (f && m->t > 0.5f) font_draw(f, "LOADING", (float)(sw - font_text_width(f, "LOADING") - 8), (float)(sh - 16), 255, 255, 255);
    }
}

/* CONTINUE? - white on black, the count in big digits shrinking away as the second runs out, the continues left
 * under it. An optional assets/continue.png (any size, letterboxed to the screen) goes behind the text. */
static void draw_continue(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    fill(r, sw, sh, 0, 0, 0, 255);
    static Sprite *bg; static bool bg_tried;
    if (!bg_tried) {
        bg_tried = true;
        const char *p = asset_path("continue.png");
        if (p) { int w, h; uint32_t *px = png_load_rgba(p, &w, &h); if (px) { bg = sprite_from_rgba(0xC0117100, px, w, h, 1); free(px); } }
    }
    if (bg) {
        float s = fminf((float)sw / bg->w, (float)sh / bg->h), w = bg->w * s, h = bg->h * s;
        sprite_draw_scaled(bg, 0, floorf((sw - w) * 0.5f), floorf((sh - h) * 0.5f), w, h);
        fill(r, sw, sh, 0, 0, 0, 96);   /* keeps the text readable over it */
    }
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    if (!f || !small) return;
    float t = m->t - 0.5f; if (t < 0) t = 0;
    int count = CONTINUE_FROM - (int)t; if (count < 0) count = 0;
    float frac = t - floorf(t);   /* the digit lands big and shrinks a touch over its second */
    const char *title = "CONTINUE ?";
    font_draw(f, title, floorf((sw - font_text_width(f, title)) * 0.5f), 48, 255, 255, 255);
    char num[16]; snprintf(num, sizeof num, "%d", count);
    float sc = 4.0f - 0.6f * frac, nw = font_text_width(f, num) * sc, nh = f->h * sc;
    font_draw_scaled(f, num, floorf((sw - nw) * 0.5f), floorf(sh * 0.5f - nh * 0.5f + 8), sc, 255, 255, 255);
    if (blink_on(0x1f, 20)) { const char *go = "PRESS START"; font_draw(small, go, floorf((sw - font_text_width(small, go)) * 0.5f), (float)(sh - 44), 255, 255, 255); }
    char left[32]; snprintf(left, sizeof left, "CREDITS %02d", m->continues_left);
    font_draw(small, left, floorf((sw - font_text_width(small, left)) * 0.5f), (float)(sh - 28), 200, 200, 200);
    if (m->t < 0.5f) fill(r, sw, sh, 0, 0, 0, clamp255((1 - m->t * 2) * 255));
}

static void draw_credits(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    /* FUN_00427a60: title backdrop, half-size logo, EXIT, one [fade] block of the credits text at a time */
    Sprite *bg = sprite_get(0xD7DEBAC0), *logo = sprite_get(0xB04BAC5F), *shadow = sprite_get(0x989121EC), *ex = sprite_get(0x94C9A3DA);
    draw_title_bg(bg, sw, sh, 1.0f);
    if (logo) {
        float lx = (sw - logo->w * 0.5f) * 0.5f;
        if (shadow) { SDL_SetTextureAlphaMod(shadow->tex, 80); sprite_draw_scaled(shadow, 0, lx, 0xc, shadow->w * 0.5f, shadow->h * 0.5f); SDL_SetTextureAlphaMod(shadow->tex, 255); }
        sprite_draw_scaled(logo, 0, lx, 0xc, logo->w * 0.5f, logo->h * 0.5f);
    }
    uint8_t R, G, B; hilite(1, &R, &G, &B);
    if (ex) sprite_draw_mod(ex, 0, (float)((sw - ex->w) / 2), 0xc5, R, G, B, 255);
    const PackEntry *e = packs_find(0x7E11BC19); Font *f = font_get(0x7405B203);
    if (!e || !f) return;
    /* page = block between [fade] markers; lines centred; <cRRGGBB> colours a line */
    char buf[12000]; size_t n = e->size < sizeof buf - 1 ? e->size : sizeof buf - 1; memcpy(buf, e->data, n); buf[n] = 0;
    int page = 0; char *lines[24]; int nl = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "[fade]", 6) || !strncmp(line, "[roll]", 6)) { if (page == m->credits_page) break; page++; nl = 0; continue; }
        if (page == m->credits_page && nl < 24) lines[nl++] = line;
    }
    while (nl > 0 && lines[nl-1][0] == 0) nl--;
    int first = 0; while (first < nl && lines[first][0] == 0) first++;
    float a = sinf(PI * m->credits_t / 3.5f) * 2.5f; if (a > 1) a = 1;
    int lh = f->h + 2, y = 0x6c - (nl - first) * lh / 2;
    for (int i = first; i < nl; i++, y += lh) {
        char *t = lines[i]; uint8_t cR = 255, cG = 255, cB = 255;
        size_t l = strlen(t); while (l && t[l-1] == '\r') t[--l] = 0;
        if (!strncmp(t, "<c", 2) && strlen(t) >= 9 && t[8] == '>') { unsigned v = (unsigned)strtoul(t + 2, NULL, 16); cR = v >> 16; cG = (v >> 8) & 255; cB = v & 255; t += 9; }
        font_draw(f, t, (float)((sw - font_text_width(f, t)) / 2), (float)y, (uint8_t)(cR * a), (uint8_t)(cG * a), (uint8_t)(cB * a));
    }
    (void)r;
}

/* MISSION ACCOMPLISHED art per cleared stage and hero (assets/victory, the original 1672x941 paintings
 * downscaled to 426x240); stage 2 (the Grand Prix) has one picture for everybody */
static Sprite *victory_art(int stage, int character)
{
    static const char *const HERO[4] = { "saber", "fireball", "april", "colt" };
    static Sprite *cache[5][4]; static bool tried[5][4];
    if (stage < 1 || stage > 4) return NULL;
    int h = stage == 2 ? 0 : character & 3;
    if (!tried[stage][h]) {
        tried[stage][h] = true;
        char name[64];
        if (stage == 2) snprintf(name, sizeof name, "victory/stage2.png");
        else snprintf(name, sizeof name, "victory/stage%d_%s.png", stage, HERO[h]);
        const char *path = asset_path(name); int w = 0, hh = 0;
        uint32_t *px = path ? png_load_rgba(path, &w, &hh) : NULL;
        if (px) { cache[stage][h] = sprite_from_rgba(0x56C70000u + (uint32_t)(stage * 4 + h), px, w, hh, 1); free(px); }
        else fprintf(stderr, "victory: missing %s\n", name);
    }
    return cache[stage][h];
}

void menu_draw(Menu *m, SDL_Renderer *r, int sw, int sh)
{
    switch (m->state) {
    case MS_SPLASH0: { fill(r, sw, sh, 0, 0, 0, 255); Sprite *s = sprite_get(0x7C5F519A); draw_centered(s, sw, s ? (sh - s->h) / 2 : 0); break; }
    case MS_SPLASH1: case MS_SPLASH2: case MS_SPLASH3: {
        /* FUN_00426030: white screen, sprite zooms in from 32x under a white veil, holds, zooms out again;
         * the last one stays at 1x after t=2 and fades to black instead */
        fill(r, sw, sh, 255, 255, 255, 255);
        static const uint32_t ids[3] = { 0x7C655085, 0x7C6B538C, 0x7C71528B };
        Sprite *s = sprite_get(ids[m->state - 1]);
        float f = ease(m->t, m->dur);
        bool tail = m->state == MS_SPLASH3 && m->t >= 2.0f;
        float sc = tail ? 1.0f : zoom_scale(f);
        if (s) sprite_draw_scaled(s, 0, (sw - s->w * sc) * 0.5f, (sh - s->h * sc) * 0.5f, s->w * sc, s->h * sc);
        if (f < 1.0f) { uint8_t v = tail ? 0 : 255; fill(r, sw, sh, v, v, v, clamp255((1 - f) * 255)); }
        break; }
    case MS_INTRO: fill(r, sw, sh, 0, 0, 0, 255); video_draw(m->video, r, sw, sh); break;
    case MS_MAIN: draw_main(m, r, sw, sh); break;
    case MS_OPTIONS: draw_options(m, r, sw, sh); break;
    case MS_BRIEFING: draw_briefing(m, r, sw, sh); break;
    case MS_CHARSEL: draw_charsel(m, r, sw, sh); break;
    case MS_CREDITS: draw_credits(m, r, sw, sh); break;
    case MS_CONTINUE: draw_continue(m, r, sw, sh); break;
    case MS_GAMEOVER: {   /* FUN_0042a310: Nemesis art + pulsing GAME OVER zoom in from 32x (period 3 s) under a black veil */
        fill(r, sw, sh, 0, 0, 0, 255);
        Sprite *bg = sprite_get(0x64981FC5), *s = sprite_get(0x24138418);
        float f = ease(m->t, m->dur), sc = zoom_scale(f);
        if (bg) sprite_draw_scaled(bg, 0, (sw - bg->w * sc) * 0.5f, (sh - bg->h * sc) * 0.5f, bg->w * sc, bg->h * sc);
        if (s) sprite_draw_mod(s, 0, (float)((sw - s->w) / 2), (float)((sh - s->h) / 2 + 0x48), 255, 255, 255, pulse_alpha());
        if (f < 1.0f) fill(r, sw, sh, 0, 0, 0, clamp255((1 - f) * 255));
        if (m->t > 2.0f) fill(r, sw, sh, 0, 0, 0, clamp255((m->t - 2.0f) / 0.9f * 255));   /* out with the music fade */
        break; }
    case MS_ACCOMPLISHED: {   /* FUN_00429e70: Fireball art + pulsing MISSION / ACCOMPLISHED zoom in from 32x over
                               * v = min(2.1 sin(pi|t|/2), 2)/2 under a black veil; t runs 0..4, then -1..0 zooms
                               * back out under a white veil (FUN_00429de0) */
        fill(r, sw, sh, 0, 0, 0, 255);
        Sprite *bg = victory_art(m->cleared_stage, m->character), *a = sprite_get(0xF6172502), *b = sprite_get(0xF629241D);
        if (!bg) bg = sprite_get(0xE963788C);   /* the demo's Fireball art when ours is missing */
        float t = m->t <= 4.0f ? m->t : m->t - 5.0f, v = 1.0f;
        if (t < 1.0f) { v = 2.1f * sinf(fabsf(t) * 1.5707964f); if (v > 2.0f) v = 2.0f; v *= 0.5f; }
        float sc = zoom_scale(v);
        if (bg) sprite_draw_scaled(bg, 0, (sw - bg->w * sc) * 0.5f, (sh - bg->h * sc) * 0.5f, bg->w * sc, bg->h * sc);
        uint8_t al = pulse_alpha();
        if (a) sprite_draw_scaled_mod(a, 0, (sw - a->w * sc) * 0.5f, (sh + 0x60 - a->h * sc) * 0.5f, a->w * sc, a->h * sc, 255, 255, 255, al);
        if (b) sprite_draw_scaled_mod(b, 0, (sw - b->w * sc) * 0.5f, (sh + 0x90 - b->h * sc) * 0.5f, b->w * sc, b->h * sc, 255, 255, 255, al);
        if (v < 1.0f) { uint8_t w = t < 0 ? 255 : 0; fill(r, sw, sh, w, w, w, clamp255((1 - v) * 255)); }
        break; }
    default: break;
    }
}
