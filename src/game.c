#include "game.h"
#include "pack.h"
#include "hud.h"
#include "audio.h"
#include "font.h"
#include "heroes.h"
#include "assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* player CRHC by selected hero (FUN_00424b20: DAT_007c5250[character - 1], so Fireball, April, Colt; Saber Rider
 * (0) falls back to the default 8403195A) */
static const uint32_t HERO_CRHC[3] = { 0x9C8F9A9E, 0x79260A58, 0x26818B85 };

static bool level_start(Game *g);
static void title_start(Game *g);
static int hearts_for(int difficulty) { return difficulty == 0 ? 4 : difficulty == 1 ? 2 : 0; }   /* FUN_00422d10 / FUN_00428840 */

bool game_init(Game *g, Ren *ren, int sw, int sh, int start_level)
{
    memset(g, 0, sizeof *g);
    g->ren = ren; g->sw = sw; g->sh = sh;
    g->menu.difficulty = 1; g->menu.lives = 2; g->menu.continues = 3; g->menu.character = 1;   /* option defaults: NORMAL, 02, 03; Fireball */
    if (plat_getenv("SABER_HERO")) g->menu.character = atoi(plat_getenv("SABER_HERO")) & 3;   /* debug: 0 Saber 1 Fireball 2 April 3 Colt */
    if (plat_getenv("SABER_LIVES")) g->menu.lives = atoi(plat_getenv("SABER_LIVES"));   /* debug: starting lives */
    g->menu.ratio = plat_default_ratio();
    if (plat_getenv("SABER_RATIO")) g->menu.ratio = atoi(plat_getenv("SABER_RATIO"));   /* debug: start in a screen ratio (0 wide, -1 4:3, 1 stretch) */
    if (plat_getenv("SABER_SCREEN")) g->menu.screen = atoi(plat_getenv("SABER_SCREEN"));   /* debug: start on a SCREEN choice (Dreamcast: 320x240 only for now) */
    if (plat_screen_43_only(g->menu.screen)) g->menu.ratio = RATIO_43;
    if (g->menu.ratio != RATIO_WIDE || g->menu.screen) {
        g->sw = g->menu.ratio == RATIO_WIDE ? plat_wide_width(g->menu.screen) : 320;
        plat_apply_screen(ren, g->sw, g->sh, g->menu.ratio, g->menu.screen ? g->menu.screen : -1);
    }
    g->stage = 1; g->continues_left = g->menu.continues;
    if (start_level) { g->stage = start_level; return level_start(g); }   /* --level N: skip the front end */
    if (plat_getenv("SABER_STAGE")) { g->stage = atoi(plat_getenv("SABER_STAGE")); if (g->stage >= 2 && g->stage <= 7) return level_start(g); }   /* debug: straight into stage 2..6 (7 = stage 6's final phase) */
    if (!plat_getenv("SABER_MENU") && (plat_getenv("SABER_START") || plat_getenv("SABER_SCRIPT"))) return level_start(g);   /* debug: straight into the level */
    if (plat_getenv("SABER_CLEARED")) g->menu.cleared_stage = atoi(plat_getenv("SABER_CLEARED"));   /* debug: SABER_MENU=15 art for that stage */
    menu_enter(&g->menu, plat_getenv("SABER_MENU") ? atoi(plat_getenv("SABER_MENU")) : MS_INTRO);
    return true;
}

static bool level_start(Game *g)
{
    Ren *ren = g->ren; int sw = g->sw, sh = g->sh;
    Menu menu = g->menu; int stage = g->stage ? g->stage : 1; int carry = g->carry_lives, conts = g->continues_left;
    bool mode7_phase2 = g->mode7_phase2;
    night_dispose(&g->night);
    forest_dispose(&g->forest);
    forest_dispose(&g->lab);
    if (g->mode7) { mode7_destroy(g->mode7); g->mode7 = NULL; }
    if (g->ramrod) { ramrod_destroy(g->ramrod); g->ramrod = NULL; }
    if (g->space) { space_destroy(g->space); g->space = NULL; }
    power_close(&g->power);
    memset(g, 0, sizeof *g);
    g->ren = ren; g->sw = sw; g->sh = sh; g->menu = menu; g->in_level = true; g->stage = stage; g->carry_lives = carry; g->continues_left = conts; g->mode7_phase2 = mode7_phase2;
    if (stage == 2) {   /* the Mode-7 Grand Prix: its own world, HUD and flow */
        music_stop();   /* finish the previous stage's jingle before the title card */
        g->mode7 = mode7_create(ren, sw, sh, g->menu.difficulty, carry > 0 ? carry : g->menu.lives, mode7_phase2);
        if (g->mode7) title_start(g);
        return g->mode7 != NULL;
    }
    if (stage == 6) {   /* Ramrod in robot mode: first person from the cockpit */
        g->ramrod = ramrod_create(ren, sw, sh, g->menu.difficulty, carry > 0 ? carry : g->menu.lives);
        if (g->ramrod) title_start(g);
        return g->ramrod != NULL;
    }
    if (stage == 7) {   /* straight on from the mechs: Ramrod in cruiser mode after the battle cruiser */
        g->space = space_create(ren, sw, sh, g->menu.difficulty, carry > 0 ? carry : g->menu.lives, g->menu.character);
        if (g->space) title_start(g);
        return g->space != NULL;
    }
    const PackEntry *t = packs_find(0x119090BF);
    uint32_t lvl = 0x12DAD1A7;
    if (t && !memcmp(t->data, "TLVL", 4)) lvl = t->data[8] | t->data[9] << 8 | t->data[10] << 16 | (uint32_t)t->data[11] << 24;
    if (!level_load(&g->level, lvl)) return false;
    /* stages 3-5 reuse level 1's block with layers of their own: their own set for a backend's planes (the Saturn's) */
    static const uint32_t PLANES_ID[6] = { 0, 0, 0, 0x4E540003u, 0x46520004u, 0x4C420005u };
    if (stage >= 3 && stage <= 5) g->level.planes_id = PLANES_ID[stage];
    g->night_on = (stage == 3);
    if (g->night_on && !night_init(&g->night, &g->level, g->menu.difficulty)) return false;
    g->forest_on = (stage == 4);
    if (g->forest_on && !forest_init(&g->forest, &g->level)) return false;
    if (g->forest_on) { memset(&g->night, 0, sizeof g->night); g->night.manual = true; night_boss_load(&g->night, &g->level, g->menu.difficulty); }   /* Hyperjumper returns in the finale */
    g->lab_on = (stage == 5);
    if (g->lab_on && !forest_load(&g->lab, &g->level, "lab", "lab.lvl", 0x4C420000u)) return false;
    if (plat_getenv("SABER_DUMPLAYERS")) level_dump_layers(&g->level, plat_getenv("SABER_DUMPLAYERS"));   /* debug: tools/saturn/layers.py */
    g->world.gx = 0; g->world.gy = R(480.0f);
    g->world.world_min_x = 0; g->world.world_max_x = g->level.width;
    real px = R(100), py = R(155);
    for (int i = 0; i < g->level.nobjs; i++) if (g->level.objs[i].type == 0) { px = g->level.objs[i].x; py = g->level.objs[i].y; }
    if (g->night_on) { px = g->night.start_x; py = g->night.start_y; }
    if (g->forest_on) { px = g->forest.start_x; py = g->forest.start_y; }
    if (g->lab_on) { px = g->lab.start_x; py = g->lab.start_y; }
    g->walk_in = (g->night_on || g->forest_on || g->lab_on) && !plat_getenv("SABER_START");   /* stages 3, 4 and 5 open with the hero walking in from the left */
    if (g->walk_in && g->night_on) { g->walk_stop_x = g->night.intro_stop_x; g->walk_script = NIGHT_SCRIPT_INTRO; }
    if (g->walk_in && g->forest_on) { px = FOREST_INTRO_CAM - R(40.0f); g->walk_stop_x = FOREST_INTRO_STOP; g->walk_script = FOREST_SCRIPT_INTRO; }
    if (g->walk_in && g->lab_on) { px = FOREST_INTRO_CAM - R(40.0f); g->walk_stop_x = FOREST_INTRO_STOP; g->walk_script = LAB_SCRIPT_INTRO; }   /* the same opening as stage 4 */
    if (plat_getenv("SABER_START")) px = r_parse(plat_getenv("SABER_START"), NULL);   /* debug */
    player_spawn(&g->player, (unsigned)(g->menu.character - 1) < 3 ? HERO_CRHC[g->menu.character - 1] : 0x8403195A, px, py);
    /* stages 3 and 4 inherit the spares left over from the stage before (a fresh --level 3/4 falls back to the option) */
    g->player.lives = (stage >= 3 && carry > 0) ? carry : g->menu.lives;
    g->player.hp = g->player.max_hp = hearts_for(g->menu.difficulty);
    power_reset(&g->power, g->menu.character, false);   /* two power attacks a stage */
    g->hero_speed = g->player.ch.speed;
    enemies_reset(&g->enemies);
    g->enemies.difficulty = g->menu.difficulty;
    g->player_layer = 11;
    for (int i = 0; i < g->level.nlayers; i++) if (!strcmp(g->level.layers[i].name, "PlayerSprites")) g->player_layer = i;
    if (g->forest_on)   /* the cabin walls: a tower sniper's shots and aimed-down rifle go over them */
        for (int i = g->player_layer + 1; i < g->level.nlayers; i++) if (!strcmp(g->level.layers[i].name, "ForegroundStuff")) { g->enemies.front_layer = i; break; }
    if (g->forest_on && g->enemies.front_layer >= 0) g->night.fx_layer = g->enemies.front_layer;   /* Hyperjumper's flashes and blasts over the towers too */
    g->enemies.aim_decks = g->forest_on;
    dialog_set_hero(g->menu.character);
    static const uint32_t DIALOG_TEXT[4] = { 0xC3B6D081, 0xC4B0D1BA, 0xC5AAD2B7, 0xC6A4D3AC };
    /* Stage 3 has its own route and enemy waves (night_level.c); none of level 1's dialogs, stampede,
     * convoy, horse or flow objects are installed. */
    if (g->night_on) {
        LevelObject tr[64];
        int n = stage3_triggers(tr, 64, g->player_layer);
        for (int i = 0; i < n; i++) enemies_add_trigger(&g->enemies, &tr[i]);
    }
    if (g->forest_on) {   /* stage 4: its own waves (forest.lvl); no exit, the clearing's finale ends it (forest.c) */
        LevelObject tr[FOREST_MAX_TRIGGERS];
        int n = forest_triggers(&g->forest, tr, FOREST_MAX_TRIGGERS, g->player_layer);
        for (int i = 0; i < n; i++) enemies_add_trigger(&g->enemies, &tr[i]);
    }
    if (g->lab_on) {   /* stage 5: its own waves (lab.lvl) and, for now, level 1's boss at the end (on its far pass layer) */
        LevelObject tr[FOREST_MAX_TRIGGERS];
        int n = forest_triggers(&g->lab, tr, FOREST_MAX_TRIGGERS, g->player_layer), far = 4;
        for (int i = 0; i < g->level.nlayers; i++) if (!strcmp(g->level.layers[i].name, "Small Hyperjmpr")) far = i;
        for (int i = 0; i < n; i++) { if (tr[i].type == 10) tr[i].layer = (uint32_t)far; enemies_add_trigger(&g->enemies, &tr[i]); }
    }
    for (int i = 0; i < g->level.nobjs && !g->night_on && !g->forest_on && !g->lab_on; i++) {
        LevelObject *o = &g->level.objs[i];
        real hx = o->wp[0][0] / 2, hy = o->wp[0][1] / 2;   /* * 0.5f */   /* +0x0c/+0x10 = zone size for flow objects */
        if (o->type >= 1 && o->type <= 29 && o->type != 3 && o->type != 4) enemies_add_trigger(&g->enemies, o);
        else if (o->type == 3 && g->ndeath < 8) { g->deathzones[g->ndeath].cx = o->x + hx; g->deathzones[g->ndeath].cy = o->y + hy; g->deathzones[g->ndeath].hx = hx; g->deathzones[g->ndeath].hy = hy; g->deathzones[g->ndeath].rx = o->spawn_x; g->deathzones[g->ndeath].ry = o->spawn_y; g->ndeath++; }
        else if (o->type == 4) { g->exit_zone.cx = o->x + hx; g->exit_zone.cy = o->y + hy; g->exit_zone.hx = hx; g->exit_zone.hy = hy; g->exit_zone.set = true; }
        else if ((o->type == 998 || o->type == 999) && g->nstops < 4) {
            int k = g->nstops++;
            g->stops[k].cx = o->x + hx; g->stops[k].cy = o->y + hy; g->stops[k].armed = true;
            g->stops[k].hx = o->type == 999 ? -hx : hx; g->stops[k].hy = o->type == 999 ? -hy : hy;
        } else if (o->type >= 1000 && o->type <= 1003) {
            int k = o->type - 1000;
            g->dialogs[k].cx = o->x + hx; g->dialogs[k].cy = o->y + hy; g->dialogs[k].hx = hx; g->dialogs[k].hy = hy;
            g->dialogs[k].text = DIALOG_TEXT[k]; g->dialogs[k].focus_x = o->wp[1][0]; g->dialogs[k].focus_y = o->wp[1][1];
            g->dialogs[k].t_in = r_milli(o->wp[2][0]); g->dialogs[k].t_out = r_milli(o->wp[2][1]);
        }
    }
    enemies_preload(&g->enemies);
    for (int k = 0; k < 4; k++) if (g->dialogs[k].text) dialog_preload(g->dialogs[k].text);
    g->state = 10;
    if (plat_getenv("SABER_DEBUG")) g->debug_collision = true;   /* debug: collision overlay from the start */
    music_play(g->night_on ? 13 : g->forest_on ? 15 : g->lab_on ? 14 : 5, true);
    g->cam_x = px - r_int(sw / 2); if (g->cam_x < 0) g->cam_x = 0;
    if (g->walk_in) g->cam_x = g->night_on ? NIGHT_INTRO_CAM : FOREST_INTRO_CAM;
    title_start(g);
    return true;
}

/* ---- the level's title card: black, an amber band sweeps open across the middle, "STAGE n" and the level's name
 * type in, then the level shows through venetian-blind strips and the last of the black fades (every level, also a
 * --level start). The world does not run under it; the music comes up with the wipe. */
#define TITLE_TEXT_T R(0.9f)    /* the band is open: the name starts typing */
#define TITLE_WIPE_T R(3.1f)    /* the strips start opening */
#define TITLE_END_T  R(4.3f)
static const struct { const char *no, *name, *sub; } TITLE[8] = {
    { "", "", "" },
    { "STAGE 1", "THE FRONTIER TOWN", "OUTRIDERS IN THE STREETS" },
    { "STAGE 2", "THE ALL GALAXY GRAND PRIX", "NEW BORDERLAND CIRCUIT" },
    { "STAGE 3", "HYPERJUMPER PASS", "OUTRIDER SKY RAID" },
    { "STAGE 4", "THE RED PALM JUNGLE", "OUTRIDER WATCHTOWERS" },
    { "STAGE 5", "THE CAVERN LABORATORY", "OUTRIDER HIDEOUT" },
    { "STAGE 6", "POWER STRIDE", "RENEGADES AT THE YUMA OUTPOST" },
    { "STAGE 6 - FINAL PHASE", "THE BATTLE CRUISER", "OUTRIDER SPACE" },
};
static int title_idx(const Game *g) { return g->stage >= 2 && g->stage <= 7 ? g->stage : 1; }
static void title_start(Game *g) { g->title_on = true; g->title_t = 0; music_set_volume(0); }
static void title_update(Game *g, real dt)
{
    real prev = g->title_t; g->title_t += dt;
    bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(&g->in, b)) any = true;
    if (any && g->title_t < TITLE_WIPE_T) g->title_t = TITLE_WIPE_T;   /* a button skips to the wipe */
    const char *name = TITLE[title_idx(g)].name; int len = (int)strlen(name);
    int shown_prev = r_trunc((prev - TITLE_TEXT_T) * 22), shown = r_trunc((g->title_t - TITLE_TEXT_T) * 22);   /* 22 letters / s */
    if (shown > shown_prev && shown <= len && shown > 0 && name[shown - 1] != ' ') sfx_play(0, 0);   /* a tick per letter */
    if (g->title_t >= TITLE_WIPE_T) music_set_volume(r_div(g->title_t - TITLE_WIPE_T, TITLE_END_T - TITLE_WIPE_T));
    if (g->title_t >= TITLE_END_T) { g->title_on = false; music_set_volume(R(1)); g->level_t = R(0.25f); }
}
static void title_draw(Game *g)
{
    real t = g->title_t; int sw = g->sw, sh = g->sh;
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    r_set_draw_blend(g->ren, R_BLEND_BLEND);
    /* the black: solid until the wipe, then 10 strips that each shrink toward their own centre line, staggered top to
     * bottom, and a thin veil that fades out last */
    if (t < TITLE_WIPE_T) { r_set_draw_color(g->ren, 0, 0, 0, 255); RFRect q = { 0, 0, r_int(sw), r_int(sh) }; r_fill_rect(g->ren, &q); }
    else {
        const int N = 10; real strip = r_int(sh) / N, w = r_div(t - TITLE_WIPE_T, TITLE_END_T - TITLE_WIPE_T);
        for (int i = 0; i < N; i++) {
            real open = PLAT_CLAMP(r_div(w - i * R(0.045f), R(0.5f)), R(0.0f), R(1.0f)); open = R(1) - r_mul(R(1) - open, R(1) - open);   /* ease out */
            real h = r_mul(strip, R(1) - open); if (h <= 0) continue;
            r_set_draw_color(g->ren, 0, 0, 0, 255); RFRect q = { 0, i * strip + (strip - h) / 2, r_int(sw), h }; r_fill_rect(g->ren, &q);
        }
        r_set_draw_color(g->ren, 0, 0, 0, (uint8_t)r_trunc(120 * (R(1) - w))); RFRect v = { 0, 0, r_int(sw), r_int(sh) }; r_fill_rect(g->ren, &v);
    }
    /* the band: a line growing from the centre (0..0.4 s), then opening vertically to 46 px (0.4..0.9 s); it slides
     * up and away with the wipe */
    real hsw = r_int(sw) / 2;   /* sw * 0.5f */
    real cy = r_int(sh) / 2 - R(2), half_w = r_mul(r_int(sw), PLAT_CLAMP(r_div(t, R(0.4f)), R(0.0f), R(1.0f))) / 2;
    real half_h = R(1.5f) + r_mul(R(21.5f), PLAT_CLAMP(r_div(t - R(0.4f), R(0.5f)), R(0.0f), R(1.0f)));
    real gone = PLAT_CLAMP(r_div(t - TITLE_WIPE_T, R(0.45f)), R(0.0f), R(1.0f)); gone = r_mul(gone, gone);
    cy -= r_mul(gone, r_int(sh) / 2 + R(40)); uint8_t fade = (uint8_t)r_trunc(255 * (R(1) - gone));
    if (t > R(0.0f) && half_w > R(1)) {
        r_set_draw_color(g->ren, 14, 22, 52, fade); RFRect band = { hsw - half_w, cy - half_h, half_w * 2, half_h * 2 }; r_fill_rect(g->ren, &band);
        r_set_draw_color(g->ren, 255, 182, 0, fade);
        RFRect top = { hsw - half_w, cy - half_h - R(2), half_w * 2, R(2) }, bot = { hsw - half_w, cy + half_h, half_w * 2, R(2) };
        r_fill_rect(g->ren, &top); r_fill_rect(g->ren, &bot);
        /* hatch marks running along the band's edges (a rolling shimmer) */
        r_set_draw_color(g->ren, 255, 230, 120, (uint8_t)r_trunc(fade * R(0.6f)));
        int off = r_trunc(t * 90) % 16;
        for (real x = hsw - half_w + r_int(off); x < hsw + half_w; x += R(16)) { RFRect m1 = { x, cy - half_h - R(2), R(6), R(2) }, m2 = { x + R(8), cy + half_h, R(6), R(2) }; r_fill_rect(g->ren, &m1); r_fill_rect(g->ren, &m2); }
    }
    if (!f || !small || t < TITLE_TEXT_T - R(0.2f)) return;
    const char *no = TITLE[title_idx(g)].no, *name = TITLE[title_idx(g)].name, *sub = TITLE[title_idx(g)].sub;
    /* "STAGE n" slides in from the left over 0.2 s */
    real sl = PLAT_CLAMP(r_div(t - (TITLE_TEXT_T - R(0.2f)), R(0.2f)), R(0.0f), R(1.0f)); sl = R(1) - r_mul(R(1) - sl, R(1) - sl);
    real x0 = hsw - r_int(font_text_width(f, name)) / 2;
    font_draw(small, no, x0 - (R(1) - sl) * 120, cy - half_h + R(4), (uint8_t)r_trunc(r_mul(255 * sl, R(1) - gone)), (uint8_t)r_trunc(r_mul(182 * sl, R(1) - gone)), 0);
    /* the name types in, a bright cursor block at its head */
    int len = (int)strlen(name), shown = r_trunc((t - TITLE_TEXT_T) * 22); if (shown < 0) shown = 0; if (shown > len) shown = len;
    font_draw_n(f, name, shown, x0, cy - R(6), fade, fade, fade);
    if (shown < len && (r_trunc(t * 12) & 1)) { r_set_draw_color(g->ren, 255, 182, 0, fade); RFRect cur = { x0 + r_int(font_text_width_n(f, name, shown)), cy - R(6), R(8), r_int(f->h) }; r_fill_rect(g->ren, &cur); }
    /* the sub line fades in once the name is complete */
    real sf = r_mul(PLAT_CLAMP(r_div(t - TITLE_TEXT_T - r_int(len) / 22 - R(0.2f), R(0.4f)), R(0.0f), R(1.0f)), R(1) - gone);
    font_draw(small, sub, hsw - r_int(font_text_width(small, sub)) / 2, cy + half_h - R(12), (uint8_t)r_trunc(200 * sf), (uint8_t)r_trunc(200 * sf), (uint8_t)r_trunc(210 * sf));
}

/* the last life is gone: CONTINUE? while credits remain (the option's count, per run), else GAME OVER */
static void game_over(Game *g)
{
    if (g->continues_left > 0) { g->menu.continues_left = g->continues_left; menu_enter(&g->menu, MS_CONTINUE); }
    else menu_enter(&g->menu, MS_GAMEOVER);
}

void game_debug_key(Game *g, int key, bool down)
{
    if (key < 0 || key >= DBG_KEY_COUNT) return;
    g->dbg_key[key] = down;
    if (down && key == DBG_KEY_COLLISION) g->debug_collision = !g->debug_collision;
    if (down && key == DBG_KEY_FREECAM) g->free_cam = !g->free_cam;
}

/* camera follows the player horizontally (FUN_0040c460), max 4 px/frame catch-up. The target is clamped to
 * [current centre, level end], so the camera never scrolls back left (flag +0x1c lifts that during a cutscene return). */
static void camera_follow(Game *g, bool allow_left)
{
    const Character *c = &g->player.ch;
    real target = c->body.x;
    real half = r_int(g->sw) / 2;   /* sw * 0.5f */
    real camc = g->cam_x + half;
    real maxc = g->level.width - half; if (target > maxc) target = maxc;
    if (!allow_left && target < camc) target = camc;
    real d = target - camc;
    if (r_abs(d) > R(4.0f)) camc += (d > 0 ? 1 : -1) * R(4.0f); else camc = target;   /* speed 1.0 * 4 (FUN_0040c460) */
    if (camc < half) camc = half;
    g->cam_x = camc - half;
}

/* a story scene from one of our own scripts: state 0xd without a focus point (the hero stops, the box opens,
 * control comes back when it closes) */
static void open_scene(Game *g, const char *script)
{
    if (!dialog_open_script(&g->dialog, script)) return;
    g->state = 0xd; g->dlg_phase = 0; g->player.locked = true;
    g->dlg_focus_x = g->dlg_focus_y = 0; g->dlg_t_before = g->dlg_t_after = 0;
}

/* a power attack's cut-in has ended: Saber's and Fireball's land now (April's and Colt's run on in power.c). Every
 * Outrider on screen goes down; a boss in the fight loses a slice of its hit points - a quarter for Fireball's blast,
 * less for Saber's slash, which also clears the enemy shots and leaves him untouchable for a moment */
static void power_strike(Game *g)
{
    int h = g->menu.character & 3;
    if (h != HERO_SABER && h != HERO_FIREBALL) return;
    bool saber = h == HERO_SABER; real frac = saber ? R(0.18f) : R(0.25f);
    enemies_power_strike(&g->enemies, &g->effects, g->cam_x, g->sw, g->sh, frac);
    if (g->night_on || g->forest_on) night_power_hit(&g->night, &g->effects, frac, saber);
    if (g->lab_on) dark_power_hit(&g->dark, frac);
    if (saber) {
        g->enemy_bullets.n = 0;
        if (g->player.ch.state != CS_DEAD) { g->player.ch.flags |= CF_HIT; g->player.ch.hit_t = R(150.0f); }
    }
}

void game_update(Game *g, real dt)
{
    input_update(&g->in);
    if (!g->in_level) {
        menu_update(&g->menu, &g->in, dt, g->sw, g->ren);
        if (g->menu.start_level) { g->menu.start_level = false; g->stage = 1; g->carry_lives = 0; g->continues_left = g->menu.continues; g->mode7_phase2 = false; level_start(g); }   /* character select always starts stage 1 */
        if (g->menu.continue_now) { g->menu.continue_now = false; g->continues_left--; g->carry_lives = 0; level_start(g); }   /* CONTINUE? taken: fresh lives, at the stage-2 pursuit if reached */
        if (g->menu.next_stage) { g->menu.next_stage = false; level_start(g); }
        return;
    }
    if (g->title_on) { title_update(g, dt); return; }
    if (g->mode7) {
        mode7_update(g->mode7, &g->in, dt);
        int res = mode7_result(g->mode7);
        if (res) {
            int lives = mode7_lives(g->mode7);
            if (res == 2) g->mode7_phase2 = mode7_phase2_reached(g->mode7);
            mode7_destroy(g->mode7); g->mode7 = NULL; g->in_level = false;
            dialog_set_hero(g->menu.character);
            if (res == 1) { g->menu.cleared_stage = g->stage; g->stage = 3; g->carry_lives = lives; g->menu.more_stages = true; menu_enter(&g->menu, MS_ACCOMPLISHED); }   /* on to Hyperjumper Pass */
            else game_over(g);
        }
        return;
    }
    if (g->ramrod) {
        ramrod_update(g->ramrod, &g->in, dt);
        int res = ramrod_result(g->ramrod);
        if (res) {
            int lives = ramrod_lives(g->ramrod);
            ramrod_destroy(g->ramrod); g->ramrod = NULL; g->in_level = false;
            dialog_set_hero(g->menu.character);
            if (res == 1) { g->stage = 7; g->carry_lives = lives; level_start(g); }   /* no result screen: the outro's radio scene leads straight into the final phase */
            else game_over(g);
        }
        return;
    }
    if (g->space) {
        space_update(g->space, &g->in, dt);
        int res = space_result(g->space);
        if (res) {
            int lives = space_lives(g->space);
            space_destroy(g->space); g->space = NULL; g->in_level = false;
            dialog_set_hero(g->menu.character);
            if (res == 1) { g->menu.cleared_stage = 7; g->stage = 8; g->carry_lives = lives; g->menu.ending = true; menu_enter(&g->menu, MS_ACCOMPLISHED); }   /* the last battle: the credits roll */
            else game_over(g);
        }
        return;
    }
    Player *p = &g->player; Character *c = &p->ch;
    if (power_in_cutin(&g->power)) {   /* a power attack's cut-in: the world holds still under it */
        power_update(&g->power, &g->in, dt);
        if (power_take_strike(&g->power)) power_strike(g);
        return;
    }
    if (g->state == 10 && btn_pressed(&g->in, BTN_POWER) && power_can_start(&g->power) && c->state != CS_DEAD && !p->locked && !g->walk_in) {
        power_start(&g->power, g->ren); p->fire_hold = true; return;
    }
    if (g->state == 0xc) {            /* pause: FUN_0042cfc0(0xc) -> FUN_00411300 pauses the music, FUN_004113b0 resumes it */
        if (btn_pressed(&g->in, BTN_PAUSE)) { g->state = 10; sfx_play(10, 0); music_pause(false); }
        return;
    }
    if (g->state == 10 && btn_pressed(&g->in, BTN_PAUSE)) { g->state = 0xc; sfx_play(10, 0); music_pause(true); return; }
    if (g->state == 10 && g->level_t < R(0.25f)) { g->level_t += dt; music_set_volume(4 * g->level_t); }   /* level music fades in (~0.25 s on a capture of the original) */
    /* FUN_0042d690: 0xb (last life lost) runs the level on with the player standing locked until 2*sin(pi*t/3) reaches 2
     * (1.5 s, the screen fades over the last 0.5 s); 0xe (mission done) plays the jingle for 5.5 s, then fades ~0.8 s */
    if (p->game_over && g->state == 10) { g->state = 0xb; g->state_t = 0; p->locked = true; }
    if (g->state == 0xb) {
        g->state_t += dt; real f = 2 * r_sin(r_mul(R(3.1415927f), g->state_t) / 3);
        if (f >= R(2.0f) || g->state_t >= R(1.5f)) { g->in_level = false; game_over(g); return; }
        music_set_volume(R(2.0f) - f);   /* the music fades with the screen (FUN_00425e70 every frame of the ramp) */
    }
    if (g->state == 0xe) {
        g->state_t += dt; real f = g->state_t > R(5.5f) ? r_mul(R(2.1f), r_sin(r_mul(g->state_t - R(5.5f), R(1.5707964f)))) : R(0.0f);
        if (f >= R(2.0f)) {
            g->in_level = false;
            if (g->stage == 1) { g->menu.cleared_stage = g->stage; g->stage = 2; g->carry_lives = g->player.lives; g->menu.more_stages = true; menu_enter(&g->menu, MS_ACCOMPLISHED); return; }   /* the Grand Prix follows */
            if (g->stage == 3) { g->menu.cleared_stage = g->stage; g->stage = 4; g->carry_lives = g->player.lives; g->menu.more_stages = true; menu_enter(&g->menu, MS_ACCOMPLISHED); return; }   /* on into the jungle */
            if (g->stage == 4) { g->menu.cleared_stage = g->stage; g->stage = 5; g->carry_lives = g->player.lives; g->menu.more_stages = true; menu_enter(&g->menu, MS_ACCOMPLISHED); return; }   /* down into the cave lab */
            if (g->stage == 5) { g->menu.cleared_stage = g->stage; g->stage = 6; g->carry_lives = g->player.lives; g->menu.more_stages = true; menu_enter(&g->menu, MS_ACCOMPLISHED); return; }   /* Ramrod to the Yuma outpost */
            g->menu.cleared_stage = g->stage; g->stage = 7; g->menu.ending = true; menu_enter(&g->menu, MS_ACCOMPLISHED); return;
        }
        music_set_volume(R(2.0f) - f);
    }
    bool cutscene_world = false;    /* state 0xd branches that still run the world (player not idle yet, timed holds) */
    if (g->state == 0xd) {          /* dialog / cutscene (FUN_0042d690, state 0xd) */
        real half = r_int(g->sw) / 2, maxx = g->level.width - r_int(g->sw);
        bool focus = g->dlg_focus_x > 0 || g->dlg_focus_y > 0;
        real target = g->dlg_focus_x - half; if (target < 0) target = 0; if (target > maxx) target = maxx;
        if (c->state != CS_IDLE) cutscene_world = true;      /* the player first comes to a stop (FUN_00422d00) */
        else if (focus) {
            if (g->dlg_phase == 2) {                          /* camera returning to the player (camera flag +0x1b) */
                if (g->cam_x == g->dlg_last_cam) { g->state = 10; p->locked = false; }
                g->dlg_last_cam = g->cam_x;
                camera_follow(g, true);
            } else if (r_trunc(g->cam_x) == r_trunc(target)) {
                if (g->dialog.active) dialog_update(&g->dialog, &g->in, dt);
                else if ((g->dlg_t_after -= dt * 1000) > 0) cutscene_world = true;   /* the scene plays on after the text */
                else { g->dlg_phase = 2; g->dlg_last_cam = -1; }
            } else {                                          /* pan to the focus point, hold just short of it for t_before */
                real prev = g->cam_x, d = target - g->cam_x;
                if (r_abs(d) <= R(4.0f)) g->cam_x = target; else g->cam_x += d > 0 ? R(4.0f) : R(-4.0f);
                if (r_trunc(g->cam_x) == r_trunc(target) && (g->dlg_t_before -= dt * 1000) > 0) { cutscene_world = true; g->cam_x = prev; }
            }
        } else {
            if (g->dialog.active) dialog_update(&g->dialog, &g->in, dt);
            else { g->state = 10; p->locked = false; }
        }
        p->fire_hold = true;   /* no shots in a dialogue, nor from the button that closed it */
        if (!cutscene_world) {
            character_animate(c, dt); effects_update(&g->effects, dt);
            for (int i = 0; i < MAX_ENEMIES; i++) if (g->enemies.e[i].cls) character_animate(&g->enemies.e[i].ch, dt);
            if (g->lab_on) dark_animate(&g->dark, dt);
            return;
        }
    }

    /* order as in GameLevel::update: controls -> (spawner, enemies) -> physics -> bullets -> camera */
    if (c->state == CS_DEAD) { c->coll = c->body.coll; player_death_update(p, dt, g->level.height, &g->cam_x, g->sw); }
    else {
        /* stages 3 and 4 open with the hero walking in from off screen on a held "right", then the radio scene */
        Input walk = { 0 };
        if (g->walk_in && g->state == 10) {
            for (int b = 0; b < BTN_COUNT; b++) walk.state[b] = 1;
            if (c->body.x < g->walk_stop_x) walk.state[BTN_RIGHT] = 0;
            else { g->walk_in = false; open_scene(g, g->walk_script); }
        }
        character_sync_ground(c); player_death_update(p, dt, g->level.height, &g->cam_x, g->sw);
        c->speed = r_mul(g->hero_speed, power_speed(&g->power) ? R(1.7f) : R(1.0f));   /* April's power: a burst of speed */
        player_control(p, g->walk_in ? &walk : &g->in, dt);
    }
    if (player_try_fire(p, &g->player_bullets, &g->effects, g->player_layer) && power_rapid(&g->power)) p->fire_cooldown = R(0.07f);   /* Colt's power */
    player_check_enemy_bullets(p, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh);
    g->world.world_min_x = g->walk_in ? 0 : g->cam_x;   /* GameLevel::update: physics world min = camera left edge (not while walking in) */
    enemies_update(&g->enemies, p, &g->level, &g->world, &g->player_bullets, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh, dt);
    if (g->night_on) {
        night_update(&g->night, p, &g->player_bullets, &g->effects, &g->level, g->cam_x, g->sw, dt, g->state == 10);
        if (g->night.boss_started) { g->enemies.spawner_enabled = false; g->cam_locked = true; }   /* the arena: no more waves, the camera stays */
        if (g->state == 10 && !g->night_taunt_done && !g->night.boss_started && c->state != CS_DEAD && c->body.x >= NIGHT_TAUNT_X) {
            g->night_taunt_done = true; open_scene(g, NIGHT_SCRIPT_TAUNT);   /* the pass patrol calls Hyperjumper in */
        }
    }
    if (g->forest_on && g->state == 10) {
        bool won = forest_finale_update(&g->forest, &g->enemies, &g->night, &g->effects, &g->level, &g->world, p, g->cam_x, g->sw, g->sh, g->player_layer, dt);
        if (g->forest.fin.state != FF_WAIT) {   /* the clearing: the camera stays (a respawn at its left edge would nudge it) */
            g->cam_locked = true; g->cam_x = g->forest.fin.arena_x;
            if (p->respawn_x < g->cam_x + R(16.0f)) p->respawn_x = g->cam_x + R(16.0f);
        }
        night_update(&g->night, p, &g->player_bullets, &g->effects, &g->level, g->cam_x, g->sw, dt, true);
        if (g->forest.fin.scene_ambush && g->state == 10) { g->forest.fin.scene_ambush = false; open_scene(g, FOREST_SCRIPT_AMBUSH); }
        else if (won && c->state != CS_DEAD && !g->forest_outro_done) { g->forest_outro_done = true; open_scene(g, FOREST_SCRIPT_OUTRO); }   /* the radio, then the win */
        else if (won && c->state != CS_DEAD && g->forest_outro_done) { g->state = 0xe; g->state_t = 0; p->locked = true; music_play(6, false); }
    }
    if (g->lab_on && plat_getenv("SABER_DARK") && g->dark.state == DA_OFF && g->state == 10 && g->cam_x >= g->level.width - r_int(g->sw) - R(1))
        dark_begin(&g->dark, g->cam_x, g->sw, g->menu.difficulty);   /* debug: SABER_DARK=1 SABER_START=6500: straight to Dark April */
    if (g->lab_on) {   /* stage 5: Dark April, once level 1's boss is down (her scenes are April's own when April is the hero) */
        dark_update(&g->dark, p, &g->in, &g->level, &g->world, &g->player_bullets, &g->enemy_bullets, &g->effects, g->player_layer, dt, g->state == 10);
        if (dark_holds_arena(&g->dark)) { g->enemies.spawner_enabled = false; g->cam_locked = true; }
        if (g->state == 10 && c->state != CS_DEAD) {
            bool april = g->menu.character == HERO_APRIL; const char *scene = NULL;
            if (g->dark.scene_call) { g->dark.scene_call = false; scene = april ? DARK_SCRIPT_CALL_APRIL : DARK_SCRIPT_CALL; }
            else if (g->dark.scene_meet) {
                g->dark.scene_meet = false; scene = april ? DARK_SCRIPT_MEET_APRIL : DARK_SCRIPT_MEET;
                const char *huh = april ? asset_path("voice/april_huh.wav") : NULL;
                if (huh) voice_play_file(huh);   /* April's own baffled "huh?" at the sight of herself */
            }
            else if (g->dark.scene_outro) { g->dark.scene_outro = false; scene = april ? DARK_SCRIPT_OUTRO_APRIL : DARK_SCRIPT_OUTRO; }
            else if (g->dark.state == DA_DONE) { g->state = 0xe; g->state_t = 0; p->locked = true; music_play(6, false); }   /* after the outro: the win */
            if (scene) {
                if (april) dialog_set_hero(HERO_FIREBALL);   /* written for April as she is: no name / avatar swap */
                open_scene(g, scene);
                dialog_set_hero(g->menu.character);
            }
        }
    }
    /* level-flow zones (FUN_00422d10 tail): exit, dialogs, camera stops, death zones */
    if (c->state != CS_DEAD && !p->locked) {
        real bx = c->body.x, by = c->body.y;
        const HurtBox *h = character_hurt(c);
        real hw = h->hw, hh = h->hh;
        #define IN_ZONE(z) (r_abs(bx - (z).cx) <= hw + r_abs((z).hx) && r_abs(by - (z).cy) <= hh + r_abs((z).hy))
        if (g->exit_zone.set && IN_ZONE(g->exit_zone)) { p->locked = true; c->flags |= CF_HIT | CF_DEAD; g->state = 0xe; g->state_t = 0; }
        else {
            bool started = false;
            for (int k = 0; k < 4 && !started; k++) {
                if (g->dialogs[k].text && !g->dialogs[k].done && IN_ZONE(g->dialogs[k])) {
                    g->dialogs[k].done = true; started = true;
                    if (dialog_open(&g->dialog, g->dialogs[k].text)) {
                        g->state = 0xd; g->dlg_phase = 0; p->locked = true;
                        g->dlg_focus_x = g->dialogs[k].focus_x; g->dlg_focus_y = g->dialogs[k].focus_y;
                        g->dlg_t_before = g->dialogs[k].t_in * 1000; g->dlg_t_after = g->dialogs[k].t_out * 1000;
                    }
                }
            }
            if (!started) for (int k = 0; k < g->nstops; k++) {
                if (!g->stops[k].armed) continue;
                if (IN_ZONE(g->stops[k])) { g->cam_locked = true; g->stops[k].armed = false; }
            }
        }
        for (int k = 0; k < g->ndeath; k++) if (IN_ZONE(g->deathzones[k])) { p->respawn_x = g->deathzones[k].rx; p->respawn_y = g->deathzones[k].ry; c->state = CS_DEAD; }
        if (by - hh > g->level.height) { p->respawn_x = p->safe_x; p->respawn_y = p->safe_y; c->state = CS_DEAD; sfx_play(15, 0); }   /* fell out of the level */
        { static int kill = -2; if (kill == -2) kill = plat_getenv("SABER_KILL") ? atoi(plat_getenv("SABER_KILL")) : -1; if (kill >= 0 && kill-- == 0) c->state = CS_DEAD; }   /* debug: die at step N */
        #undef IN_ZONE
    }
    if (g->enemies.release_request) {
        g->enemies.release_request = false; g->cam_locked = false;
        for (int k = 0; k < g->nstops; k++) if (g->stops[k].armed && g->stops[k].cx + g->stops[k].hx > g->cam_x && g->stops[k].cx + g->stops[k].hx < g->cam_x + r_int(g->sw)) g->stops[k].armed = false;
    }
    if (g->enemies.cam_locked) g->cam_locked = true;
    if (g->enemies.boss_done && g->state == 10) {
        g->enemies.boss_done = false;
        if (g->lab_on && !plat_getenv("SABER_NODARK")) { if (g->dark.state == DA_OFF) dark_begin(&g->dark, g->cam_x, g->sw, g->menu.difficulty); }   /* stage 5: not over yet (Dark April ends it) */
        else { g->state = 0xe; g->state_t = 0; p->locked = true; music_play(6, false); }
    }
    if (g->night_on && g->night.clear_ready && g->state == 10 && !g->night_outro_done && c->state != CS_DEAD) { g->night_outro_done = true; open_scene(g, NIGHT_SCRIPT_OUTRO); }
    if (g->night_on && g->night.clear_ready && g->state == 10 && g->night_outro_done) { g->night.clear_ready = false; g->state = 0xe; g->state_t = 0; p->locked = true; music_play(6, false); }
    player_resolve(c, dt);
    if (g->cam_locked && c->body.vx > 0 && g->cam_x + r_int(g->sw) - c->body.hx < c->body.x) c->body.vx = 0;
    physics_step(&g->world, &g->level, &c->body, dt);
    bullets_update(&g->player_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    bullets_update(&g->enemy_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    character_animate(c, dt);
    if (power_speed(&g->power)) character_animate(c, r_mul(dt, R(0.7f)));   /* the legs keep up with April's burst */
    power_update(&g->power, &g->in, dt);
    power_trail_update(&g->power, c, dt);
    effects_update(&g->effects, dt);
    player_frame_end(p, dt);
    static int trace = -1; if (trace < 0) trace = plat_getenv("SABER_TRACE") ? (plat_getenv("SABER_TRACE")[0] == '3' ? 3 : 1) : 0;
    if (trace == 3) {   /* debug: SABER_TRACE=3, every step precisely (comparing arithmetic changes, e.g. fx.h, between builds) */
        fprintf(stderr, "T st=%d pos=%s,%s v=%s,%s cam=%s,%s hp=%d e=%d", c->state, RS(c->body.x, 4), RS(c->body.y, 4), RS(c->body.vx, 4), RS(c->body.vy, 4), RS(g->cam_x, 4), RS(g->cam_y, 4), p->hp, g->enemies.count);
        for (int i = 0; i < MAX_ENEMIES; i++) if (g->enemies.e[i].cls) fprintf(stderr, " %d:%s,%s", g->enemies.e[i].cls, RS(g->enemies.e[i].ch.body.x, 3), RS(g->enemies.e[i].ch.body.y, 3));
        fprintf(stderr, " b=%d/%d\n", g->player_bullets.n, g->enemy_bullets.n);
    } else if (trace) fprintf(stderr, "cam=%s lock=%d st=%d aim=%d face=%d anim=%d frame=%d ov=%d ovf=%d flags=%x coll=%x pos=%s,%s v=%s,%s in=%d%d%d%d%d%d%d\n", RS(g->cam_x, 0), g->cam_locked, c->state, c->aim, c->facing, c->anim, c->frame, c->overlay, c->ov_frame, c->flags, c->coll, RS(c->body.x, 1), RS(c->body.y, 1), RS(c->body.vx, 1), RS(c->body.vy, 1),
        g->in.state[0], g->in.state[1], g->in.state[2], g->in.state[3], g->in.state[4], g->in.state[5], g->in.state[6]);

    if (g->free_cam) {
        real sp = r_mul_dt(g->dbg_key[DBG_KEY_FAST] ? R(600.f) : R(200.f), dt);
        if (g->dbg_key[DBG_KEY_RIGHT]) g->cam_x += sp;
        if (g->dbg_key[DBG_KEY_LEFT]) g->cam_x -= sp;
    } else if (g->cam_locked || g->state == 0xd) {
        /* camera frozen during a stop / driven by the cutscene */
    } else camera_follow(g, false);
    real maxx = g->level.width - r_int(g->sw); if (maxx < 0) maxx = 0;
    if (g->cam_x < 0) g->cam_x = 0;
    if (g->cam_x > maxx) g->cam_x = maxx;
    /* the platform stages were made for a 240-line view from the level's top; a shorter screen (the Saturn's 224 lines,
       plan 3) keeps the bottom of that view (the ground, the heroes' feet) and loses the top of the sky */
    g->cam_y = g->sh < 240 ? r_int(240 - g->sh) : R(0.0f);
}

static void draw_collision(Game *g)
{
    Level *L = &g->level;
    r_set_draw_blend(g->ren, R_BLEND_BLEND);
    int cx0 = r_trunc(g->cam_x / L->cellw), cy0 = r_trunc(g->cam_y / L->cellh);
    for (int cy = cy0; cy < cy0 + g->sh / L->cellh + 2; cy++)
        for (int cx = cx0; cx < cx0 + g->sw / L->cellw + 2; cx++) {
            uint8_t v = level_cell(L, cx, cy);
            if (!v) continue;
            if (v == 15) r_set_draw_color(g->ren, 255, 0, 0, 90); else r_set_draw_color(g->ren, 0, 255, 0, 90);
            RFRect r = { r_int(cx * L->cellw) - g->cam_x, r_int(cy * L->cellh) - g->cam_y, r_int(L->cellw), r_int(L->cellh) };
            r_fill_rect(g->ren, &r);
        }
    Body *b = &g->player.ch.body;
    r_set_draw_color(g->ren, 0, 200, 255, 160);
    RFRect r = { b->x + b->ox - b->hx - g->cam_x, b->y + b->oy - b->hy - g->cam_y, b->hx * 2, b->hy * 2 };
    r_rect(g->ren, &r);
    r_set_draw_color(g->ren, 255, 255, 0, 200);
    for (int i = 0; i < L->nobjs && !g->night_on && !g->forest_on; i++) {
        LevelObject *o = &L->objs[i];
        RFRect q = { o->x - g->cam_x - R(2), r_int((o->type >= 998 ? 20 : 8) + (int)(o->type % 7) * 6), R(4), R(4) };
        r_fill_rect(g->ren, &q);
    }
}

static void draw_hero(Game *g)
{
    power_draw_trail(&g->power, g->cam_x, g->cam_y);   /* April's afterimages under her */
    character_draw(&g->player.ch, g->cam_x, g->cam_y);
}

static void draw_scanlines(Game *g)
{
    if (menu_scanlines(&g->menu)) gfx_scanlines(g->sw, g->sh);
}

void game_draw(Game *g)
{
    if (g->menu.apply_screen_mode && !g->in_level) {
        g->menu.apply_screen_mode = false;
        g->sw = g->menu.ratio == RATIO_WIDE ? plat_wide_width(g->menu.screen) : 320;
        plat_apply_screen(g->ren, g->sw, g->sh, g->menu.ratio, g->menu.screen);
    }
    if (!g->in_level) { menu_draw(&g->menu, g->ren, g->sw, g->sh); draw_scanlines(g); return; }
    if (g->mode7) { mode7_draw(g->mode7, menu_scanlines(&g->menu)); if (g->title_on) title_draw(g); return; }
    if (g->ramrod) { ramrod_draw(g->ramrod, menu_scanlines(&g->menu)); if (g->title_on) title_draw(g); return; }
    if (g->space) { space_draw(g->space, menu_scanlines(&g->menu)); if (g->title_on) title_draw(g); return; }
    Level *L = &g->level;
    bool power_shake = g->power.phase == PW_FLASH && g->power.t < R(0.35f) && !g->power.bomb;
    real shake = g->enemies.cam_shake || power_shake ? r_int(rand() % 4) : R(0.0f);
    real saved = g->cam_y; g->cam_y += shake;
    if (g->night_on) { r_set_depth(g->ren, 0); night_draw_background(&g->night, g->cam_x, g->sw, g->sh); }   /* night sky + red moon, behind every layer (and every hardware plane) */
    bool hero_drawn = false;
    const Body *hb = &g->player.ch.body;
    bool in_cabin = g->forest_on && forest_on_deck(L, hb->x, hb->y + hb->oy + hb->hy);
    for (int i = 0; i < L->nlayers; i++) {
        r_set_depth(g->ren, i);   /* the sprites drawn now sit at this layer (render.h: for hardware planes) */
        if (L->layers[i].is_tilemap) {
            CBlock *cb = L->layers[i].map ? L->layers[i].map->cb : NULL;
            uint8_t r, gr, b;
            if (g->night_on && cb) {   /* stage 3: every level-1 layer, in the night palette */
                if (!night_layer_tint(&g->night, &L->layers[i], &r, &gr, &b)) continue;
                cblock_tint(cb, r, gr, b);
                level_draw_layer(L, i, g->cam_x, g->cam_y, g->sw, g->sh);
                cblock_tint(cb, 255, 255, 255);
            } else level_draw_layer(L, i, g->cam_x, g->cam_y, g->sw, g->sh);
            /* stage 4: Hyperjumper flies over the towers, not behind their walls */
            if (g->forest_on && i == g->enemies.front_layer) night_draw_layer(&g->night, g->ren, g->night.play_layer, g->cam_x, g->cam_y);
            /* stage 4: the tower railings hide the gunmen in the cabins, and the hero too once he is up on a deck;
               jumping up through one from below he stays in front (half a hero behind a plank, the rest in front
               of the tower, read as the sprite being cut) */
            if (g->forest_on && !hero_drawn && i > g->player_layer && !strcmp(L->layers[i].name, "ForegroundStuff")) {
                if (g->state != 0xb) draw_hero(g);
                hero_drawn = true;
            }
            if (i == g->enemies.front_layer) {   /* stage 4: a tower sniper's rifle over his cabin wall, his shots and flash */
                enemies_draw_front(&g->enemies, g->cam_x, g->cam_y);
                bullets_draw(&g->enemy_bullets, i, g->cam_x, g->cam_y);
                effects_draw(&g->effects, i, g->cam_x, g->cam_y);
            }
        }
        else {
            enemies_draw(&g->enemies, i, g->cam_x, g->cam_y);
            if (g->night_on || (g->forest_on && (i != g->night.play_layer || g->enemies.front_layer < 0)))
                night_draw_layer(&g->night, g->ren, i, g->cam_x, g->cam_y);   /* Hyperjumper's passes use the level-1 boss layers */
            if (i == g->player_layer && g->lab_on) dark_draw(&g->dark, g->cam_x, g->cam_y);   /* stage 5: Dark April, behind the hero */
            if (i == g->player_layer && (!g->forest_on || in_cabin) && g->state != 0xb) draw_hero(g);
            if (i == g->player_layer && in_cabin) hero_drawn = true;   /* the last life is gone: no respawned hero standing there during the fade */
            bullets_draw(&g->player_bullets, i, g->cam_x, g->cam_y);
            bullets_draw(&g->enemy_bullets, i, g->cam_x, g->cam_y);
            effects_draw(&g->effects, i, g->cam_x, g->cam_y);
        }
    }
    if (g->forest_on && !hero_drawn && g->state != 0xb) draw_hero(g);
    r_set_depth(g->ren, -1);
    g->cam_y = saved;
    hud_draw(g->ren, g->menu.character, g->menu.difficulty, g->player.lives, g->player.hp, g->power.items);   /* the item count = power attacks left */
    power_draw_hud(&g->power, g->ren, R(0x2c), R(0x17), R(0x1a));
    if (g->state == 0xd) dialog_draw(&g->dialog, g->ren, g->sw, g->sh);
    if (g->night_on || g->forest_on) night_draw_hud(&g->night, g->ren, g->sw, g->sh);
    if (g->lab_on && g->state != 0xd) dark_draw_hud(&g->dark, g->ren, g->sw);
    if (g->forest_on && g->state != 0xd) forest_finale_draw_hud(&g->forest, &g->enemies, &g->night, g->ren, g->sw);
    if (g->state == 0xc) {   /* pause: dim + blinking PAUSE sprite (B2143E42) */
        r_set_draw_blend(g->ren, R_BLEND_BLEND); r_set_draw_color(g->ren, 0, 0, 0, 64);
        RFRect q = { 0, 0, r_int(g->sw), r_int(g->sh) }; r_fill_rect(g->ren, &q);
        Sprite *ps = sprite_get(0xB2143E42);
        if (ps && ((plat_ticks_ms() / 16) & 0x7f) > 0x30) sprite_draw(ps, 0, r_int((g->sw - ps->w) / 2), r_int((g->sh - ps->h) / 2), false);
    }
    power_draw(&g->power, g->ren, g->sw, g->sh);
    draw_scanlines(g);
    if (g->title_on) title_draw(g);
    if (g->state == 0xe || g->state == 0xb) {   /* fade out: the same sine ramps that end the states, minus 1 */
        real a = g->state == 0xb ? 2 * r_sin(r_mul(R(3.1415927f), g->state_t) / 3) - R(1.0f)
                                  : (g->state_t > R(5.5f) ? r_mul(R(2.1f), r_sin(r_mul(g->state_t - R(5.5f), R(1.5707964f)))) - R(1.0f) : R(0.0f));
        if (a > R(1)) a = R(1);
        if (a < 0) a = 0;
        r_set_draw_blend(g->ren, R_BLEND_BLEND); r_set_draw_color(g->ren, g->state == 0xe ? 255 : 0, g->state == 0xe ? 255 : 0, g->state == 0xe ? 255 : 0, (uint8_t)r_trunc(a * 255));
        RFRect q = { 0, 0, r_int(g->sw), r_int(g->sh) }; r_fill_rect(g->ren, &q);
    }
    if (g->debug_collision) draw_collision(g);
}
