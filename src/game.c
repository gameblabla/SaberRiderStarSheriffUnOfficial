#include "game.h"
#include "pack.h"
#include "hud.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* player CRHC per selected hero (DAT_007c5250): Fireball, Saber Rider, Colt; default 8403195A */
static const uint32_t HERO_CRHC[3] = { 0x9C8F9A9E, 0x79260A58, 0x26818B85 };

static bool level_start(Game *g);

bool game_init(Game *g, SDL_Renderer *ren, int sw, int sh)
{
    memset(g, 0, sizeof *g);
    g->ren = ren; g->sw = sw; g->sh = sh;
    g->menu.difficulty = 1; g->menu.lives = 2; g->menu.continues = 3;   /* option defaults: NORMAL, 02, 03 */
    if (!SDL_getenv("SABER_MENU") && (SDL_getenv("SABER_START") || SDL_getenv("SABER_SCRIPT"))) return level_start(g);   /* debug: straight into the level */
    menu_enter(&g->menu, SDL_getenv("SABER_MENU") ? atoi(SDL_getenv("SABER_MENU")) : MS_SPLASH0);
    return true;
}

static bool level_start(Game *g)
{
    SDL_Renderer *ren = g->ren; int sw = g->sw, sh = g->sh;
    Menu menu = g->menu;
    memset(g, 0, sizeof *g);
    g->ren = ren; g->sw = sw; g->sh = sh; g->menu = menu; g->in_level = true;
    const PackEntry *t = packs_find(0x119090BF);
    uint32_t lvl = 0x12DAD1A7;
    if (t && !memcmp(t->data, "TLVL", 4)) lvl = t->data[8] | t->data[9] << 8 | t->data[10] << 16 | (uint32_t)t->data[11] << 24;
    if (!level_load(&g->level, lvl)) return false;
    g->world.gx = 0; g->world.gy = 480.0f;
    g->world.world_min_x = 0; g->world.world_max_x = g->level.width;
    float px = 100, py = 155;
    for (int i = 0; i < g->level.nobjs; i++) if (g->level.objs[i].type == 0) { px = g->level.objs[i].x; py = g->level.objs[i].y; }
    if (SDL_getenv("SABER_START")) px = (float)atof(SDL_getenv("SABER_START"));   /* debug */
    player_spawn(&g->player, HERO_CRHC[0], px, py);
    g->player.lives = g->menu.lives;
    enemies_reset(&g->enemies);
    static const uint32_t DIALOG_TEXT[4] = { 0xC3B6D081, 0xC4B0D1BA, 0xC5AAD2B7, 0xC6A4D3AC };
    for (int i = 0; i < g->level.nobjs; i++) {
        LevelObject *o = &g->level.objs[i];
        float hx = o->wp[0][0] * 0.5f, hy = o->wp[0][1] * 0.5f;   /* +0x0c/+0x10 = zone size for flow objects */
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
            g->dialogs[k].t_in = o->wp[2][0] * 0.001f; g->dialogs[k].t_out = o->wp[2][1] * 0.001f;
        }
    }
    g->state = 10;
    if (SDL_getenv("SABER_DEBUG")) g->debug_collision = true;   /* debug: collision overlay from the start */
    music_play(5, true);
    g->player_layer = 11;
    for (int i = 0; i < g->level.nlayers; i++) if (!strcmp(g->level.layers[i].name, "PlayerSprites")) g->player_layer = i;
    g->cam_x = px - sw / 2; if (g->cam_x < 0) g->cam_x = 0;
    return true;
}

void game_event(Game *g, const SDL_Event *ev)
{
    input_event(&g->in, ev);
    if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) {
        g->key[ev->key.scancode] = ev->type == SDL_EVENT_KEY_DOWN;
        if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.scancode == SDL_SCANCODE_F1) g->debug_collision = !g->debug_collision;
        if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.scancode == SDL_SCANCODE_F2) g->free_cam = !g->free_cam;
    }
}

/* camera follows the player horizontally (FUN_0040c460), max 4 px/frame catch-up. The target is clamped to
 * [current centre, level end], so the camera never scrolls back left (flag +0x1c lifts that during a cutscene return). */
static void camera_follow(Game *g, bool allow_left)
{
    const Character *c = &g->player.ch;
    float target = c->body.x;
    float half = g->sw * 0.5f;
    float camc = g->cam_x + half;
    float maxc = g->level.width - half; if (target > maxc) target = maxc;
    if (!allow_left && target < camc) target = camc;
    float d = target - camc;
    if (fabsf(d) > 4.0f) camc += (d > 0 ? 1 : -1) * 4.0f; else camc = target;   /* speed 1.0 * 4 (FUN_0040c460) */
    if (camc < half) camc = half;
    g->cam_x = camc - half;
}

void game_update(Game *g, float dt)
{
    input_update(&g->in);
    if (!g->in_level) {
        menu_update(&g->menu, &g->in, dt, g->sw, g->ren);
        if (g->menu.start_level) { g->menu.start_level = false; level_start(g); }
        return;
    }
    Player *p = &g->player; Character *c = &p->ch;
    if (g->state == 0xc) {            /* pause */
        if (btn_pressed(&g->in, BTN_PAUSE)) { g->state = 10; sfx_play(10, 0); }
        return;
    }
    if (g->state == 10 && btn_pressed(&g->in, BTN_PAUSE)) { g->state = 0xc; sfx_play(10, 0); return; }
    /* FUN_0042d690: 0xb (last life lost) runs the level on with the player standing locked until 2*sin(pi*t/3) reaches 2
     * (1.5 s, the screen fades over the last 0.5 s); 0xe (mission done) plays the jingle for 5.5 s, then fades ~0.8 s */
    if (p->game_over && g->state == 10) { g->state = 0xb; g->state_t = 0; p->locked = true; }
    if (g->state == 0xb) { g->state_t += dt; if (2.0f * sinf(3.1415927f * g->state_t / 3.0f) >= 2.0f || g->state_t >= 1.5f) { g->in_level = false; menu_enter(&g->menu, MS_GAMEOVER); return; } }
    if (g->state == 0xe) { g->state_t += dt; if (g->state_t > 5.5f && 2.1f * sinf((g->state_t - 5.5f) * 1.5707964f) >= 2.0f) { g->in_level = false; menu_enter(&g->menu, MS_ACCOMPLISHED); return; } }
    bool cutscene_world = false;    /* state 0xd branches that still run the world (player not idle yet, timed holds) */
    if (g->state == 0xd) {          /* dialog / cutscene (FUN_0042d690, state 0xd) */
        float half = g->sw * 0.5f, maxx = g->level.width - g->sw;
        bool focus = g->dlg_focus_x > 0 || g->dlg_focus_y > 0;
        float target = g->dlg_focus_x - half; if (target < 0) target = 0; if (target > maxx) target = maxx;
        if (c->state != CS_IDLE) cutscene_world = true;      /* the player first comes to a stop (FUN_00422d00) */
        else if (focus) {
            if (g->dlg_phase == 2) {                          /* camera returning to the player (camera flag +0x1b) */
                if (g->cam_x == g->dlg_last_cam) { g->state = 10; p->locked = false; }
                g->dlg_last_cam = g->cam_x;
                camera_follow(g, true);
            } else if ((int)g->cam_x == (int)target) {
                if (g->dialog.active) dialog_update(&g->dialog, &g->in, dt);
                else if ((g->dlg_t_after -= dt * 1000.0f) > 0) cutscene_world = true;   /* the scene plays on after the text */
                else { g->dlg_phase = 2; g->dlg_last_cam = -1; }
            } else {                                          /* pan to the focus point, hold just short of it for t_before */
                float prev = g->cam_x, d = target - g->cam_x;
                if (fabsf(d) <= 4.0f) g->cam_x = target; else g->cam_x += d > 0 ? 4.0f : -4.0f;
                if ((int)g->cam_x == (int)target && (g->dlg_t_before -= dt * 1000.0f) > 0) { cutscene_world = true; g->cam_x = prev; }
            }
        } else {
            if (g->dialog.active) dialog_update(&g->dialog, &g->in, dt);
            else { g->state = 10; p->locked = false; }
        }
        if (!cutscene_world) {
            character_animate(c, dt); effects_update(&g->effects, dt);
            for (int i = 0; i < MAX_ENEMIES; i++) if (g->enemies.e[i].cls) character_animate(&g->enemies.e[i].ch, dt);
            return;
        }
    }

    /* order as in GameLevel::update: controls -> (spawner, enemies) -> physics -> bullets -> camera */
    if (c->state == CS_DEAD) { c->coll = c->body.coll; player_death_update(p, dt, g->level.height, &g->cam_x, g->sw); }
    else { character_sync_ground(c); player_death_update(p, dt, g->level.height, &g->cam_x, g->sw); player_control(p, &g->in, dt); }
    player_try_fire(p, &g->player_bullets, &g->effects, g->player_layer);
    player_check_enemy_bullets(p, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh);
    g->world.world_min_x = g->cam_x;   /* GameLevel::update: physics world min = camera left edge */
    enemies_update(&g->enemies, p, &g->level, &g->world, &g->player_bullets, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh, dt);
    /* level-flow zones (FUN_00422d10 tail): exit, dialogs, camera stops, death zones */
    if (c->state != CS_DEAD && !p->locked) {
        float bx = c->body.x, by = c->body.y;
        const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
        float hw = h->hw, hh = h->hh;
        #define IN_ZONE(z) (fabsf(bx - (z).cx) <= hw + fabsf((z).hx) && fabsf(by - (z).cy) <= hh + fabsf((z).hy))
        if (g->exit_zone.set && IN_ZONE(g->exit_zone)) { p->locked = true; c->flags |= CF_HIT | CF_DEAD; g->state = 0xe; g->state_t = 0; }
        else {
            bool started = false;
            for (int k = 0; k < 4 && !started; k++) {
                if (g->dialogs[k].text && !g->dialogs[k].done && IN_ZONE(g->dialogs[k])) {
                    g->dialogs[k].done = true; started = true;
                    if (dialog_open(&g->dialog, g->dialogs[k].text)) {
                        g->state = 0xd; g->dlg_phase = 0; p->locked = true;
                        g->dlg_focus_x = g->dialogs[k].focus_x; g->dlg_focus_y = g->dialogs[k].focus_y;
                        g->dlg_t_before = g->dialogs[k].t_in * 1000.0f; g->dlg_t_after = g->dialogs[k].t_out * 1000.0f;
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
        #undef IN_ZONE
    }
    if (g->enemies.release_request) {
        g->enemies.release_request = false; g->cam_locked = false;
        for (int k = 0; k < g->nstops; k++) if (g->stops[k].armed && g->stops[k].cx + g->stops[k].hx > g->cam_x && g->stops[k].cx + g->stops[k].hx < g->cam_x + g->sw) g->stops[k].armed = false;
    }
    if (g->enemies.cam_locked) g->cam_locked = true;
    if (g->enemies.boss_done && g->state == 10) { g->enemies.boss_done = false; g->state = 0xe; g->state_t = 0; p->locked = true; music_play(6, false); }
    player_resolve(c, dt);
    if (g->cam_locked && c->body.vx > 0 && g->cam_x + g->sw - c->body.hx < c->body.x) c->body.vx = 0;
    physics_step(&g->world, &g->level, &c->body, dt);
    bullets_update(&g->player_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    bullets_update(&g->enemy_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    character_animate(c, dt);
    effects_update(&g->effects, dt);
    player_frame_end(p, dt);
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "cam=%.0f lock=%d st=%d aim=%d face=%d anim=%d frame=%d flags=%x coll=%x pos=%.1f,%.1f v=%.1f,%.1f in=%d%d%d%d%d%d%d\n",
        g->cam_x, g->cam_locked, c->state, c->aim, c->facing, c->anim, c->frame, c->flags, c->coll, c->body.x, c->body.y, c->body.vx, c->body.vy,
        g->in.state[0], g->in.state[1], g->in.state[2], g->in.state[3], g->in.state[4], g->in.state[5], g->in.state[6]);

    if (g->free_cam) {
        float sp = (g->key[SDL_SCANCODE_LSHIFT] ? 600.f : 200.f) * dt;
        if (g->key[SDL_SCANCODE_RIGHT]) g->cam_x += sp;
        if (g->key[SDL_SCANCODE_LEFT]) g->cam_x -= sp;
    } else if (g->cam_locked || g->state == 0xd) {
        /* camera frozen during a stop / driven by the cutscene */
    } else camera_follow(g, false);
    float maxx = g->level.width - g->sw; if (maxx < 0) maxx = 0;
    if (g->cam_x < 0) g->cam_x = 0;
    if (g->cam_x > maxx) g->cam_x = maxx;
    g->cam_y = 0;
}

static void draw_collision(Game *g)
{
    Level *L = &g->level;
    SDL_SetRenderDrawBlendMode(g->ren, SDL_BLENDMODE_BLEND);
    int cx0 = (int)(g->cam_x / L->cellw), cy0 = (int)(g->cam_y / L->cellh);
    for (int cy = cy0; cy < cy0 + g->sh / L->cellh + 2; cy++)
        for (int cx = cx0; cx < cx0 + g->sw / L->cellw + 2; cx++) {
            uint8_t v = level_cell(L, cx, cy);
            if (!v) continue;
            if (v == 15) SDL_SetRenderDrawColor(g->ren, 255, 0, 0, 90); else SDL_SetRenderDrawColor(g->ren, 0, 255, 0, 90);
            SDL_FRect r = { cx * L->cellw - g->cam_x, cy * L->cellh - g->cam_y, (float)L->cellw, (float)L->cellh };
            SDL_RenderFillRect(g->ren, &r);
        }
    Body *b = &g->player.ch.body;
    SDL_SetRenderDrawColor(g->ren, 0, 200, 255, 160);
    SDL_FRect r = { b->x + b->ox - b->hx - g->cam_x, b->y + b->oy - b->hy - g->cam_y, b->hx * 2, b->hy * 2 };
    SDL_RenderRect(g->ren, &r);
    SDL_SetRenderDrawColor(g->ren, 255, 255, 0, 200);
    for (int i = 0; i < L->nobjs; i++) {
        LevelObject *o = &L->objs[i];
        SDL_FRect q = { o->x - g->cam_x - 2, (o->type >= 998 ? 20 : 8) + (o->type % 7) * 6.f, 4, 4 };
        SDL_RenderFillRect(g->ren, &q);
    }
}

static void draw_scanlines(Game *g)
{
    if (!menu_scanlines(&g->menu)) return;
    SDL_SetRenderDrawBlendMode(g->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(g->ren, 0, 0, 0, 70);
    for (int y = 1; y < g->sh; y += 2) { SDL_FRect q = { 0, (float)y, (float)g->sw, 1 }; SDL_RenderFillRect(g->ren, &q); }
}

void game_draw(Game *g)
{
    if (g->menu.apply_screen_mode && !g->in_level) {
        g->menu.apply_screen_mode = false;
        g->sw = g->menu.ratio == RATIO_WIDE ? 426 : 320;
        SDL_SetRenderLogicalPresentation(g->ren, g->sw, g->sh, g->menu.ratio == RATIO_STRETCH ? SDL_LOGICAL_PRESENTATION_STRETCH : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        SDL_Window *win = SDL_GetRenderWindow(g->ren);
        if (win) {
            SDL_SetWindowFullscreen(win, g->menu.screen == 0);
            if (g->menu.screen > 0) SDL_SetWindowSize(win, 426 * (g->menu.screen + 1), 240 * (g->menu.screen + 1));
        }
    }
    if (!g->in_level) { menu_draw(&g->menu, g->ren, g->sw, g->sh); draw_scanlines(g); return; }
    Level *L = &g->level;
    float shake = g->enemies.cam_shake ? (float)(rand() % 4) : 0.0f;
    float saved = g->cam_y; g->cam_y += shake;
    for (int i = 0; i < L->nlayers; i++) {
        if (L->layers[i].is_tilemap) level_draw_layer(L, i, g->cam_x, g->cam_y, g->sw, g->sh);
        else {
            enemies_draw(&g->enemies, i, g->cam_x, g->cam_y);
            if (i == g->player_layer) character_draw(&g->player.ch, g->cam_x, g->cam_y);
            bullets_draw(&g->player_bullets, i, g->cam_x, g->cam_y);
            bullets_draw(&g->enemy_bullets, i, g->cam_x, g->cam_y);
            effects_draw(&g->effects, i, g->cam_x, g->cam_y);
        }
    }
    g->cam_y = saved;
    hud_draw(g->ren, 0, 1, g->player.lives, g->player.hp, 0);
    if (g->state == 0xd) dialog_draw(&g->dialog, g->ren, g->sw, g->sh);
    if (g->state == 0xc) {   /* pause: dim + blinking PAUSE sprite (B2143E42) */
        SDL_SetRenderDrawBlendMode(g->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(g->ren, 0, 0, 0, 64);
        SDL_FRect q = { 0, 0, (float)g->sw, (float)g->sh }; SDL_RenderFillRect(g->ren, &q);
        Sprite *ps = sprite_get(0xB2143E42);
        if (ps && ((SDL_GetTicks() / 16) & 0x7f) > 0x30) sprite_draw(ps, 0, (float)((g->sw - ps->w) / 2), (float)((g->sh - ps->h) / 2), false);
    }
    draw_scanlines(g);
    if (g->state == 0xe || g->state == 0xb) {   /* fade out: the same sine ramps that end the states, minus 1 */
        float a = g->state == 0xb ? 2.0f * sinf(3.1415927f * g->state_t / 3.0f) - 1.0f
                                  : (g->state_t > 5.5f ? 2.1f * sinf((g->state_t - 5.5f) * 1.5707964f) - 1.0f : 0.0f);
        if (a > 1) a = 1; if (a < 0) a = 0;
        SDL_SetRenderDrawBlendMode(g->ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(g->ren, g->state == 0xe ? 255 : 0, g->state == 0xe ? 255 : 0, g->state == 0xe ? 255 : 0, (uint8_t)(a * 255));
        SDL_FRect q = { 0, 0, (float)g->sw, (float)g->sh }; SDL_RenderFillRect(g->ren, &q);
    }
    if (g->debug_collision) draw_collision(g);
}
