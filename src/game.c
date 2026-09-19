#include "game.h"
#include "pack.h"
#include "hud.h"
#include <stdio.h>
#include <math.h>

/* player CRHC per selected hero (DAT_007c5250): Fireball, Saber Rider, Colt; default 8403195A */
static const uint32_t HERO_CRHC[3] = { 0x9C8F9A9E, 0x79260A58, 0x26818B85 };

bool game_init(Game *g, SDL_Renderer *ren, int sw, int sh)
{
    memset(g, 0, sizeof *g);
    g->ren = ren; g->sw = sw; g->sh = sh;
    const PackEntry *t = packs_find(0x119090BF);
    uint32_t lvl = 0x12DAD1A7;
    if (t && !memcmp(t->data, "TLVL", 4)) lvl = t->data[8] | t->data[9] << 8 | t->data[10] << 16 | (uint32_t)t->data[11] << 24;
    if (!level_load(&g->level, lvl)) return false;
    g->world.gx = 0; g->world.gy = 480.0f;
    g->world.world_min_x = 0; g->world.world_max_x = g->level.width;
    float px = 100, py = 155;
    for (int i = 0; i < g->level.nobjs; i++) if (g->level.objs[i].type == 0) { px = g->level.objs[i].x; py = g->level.objs[i].y; }
    player_spawn(&g->player, HERO_CRHC[0], px, py);
    enemies_reset(&g->enemies);
    for (int i = 0; i < g->level.nobjs; i++) {
        LevelObject *o = &g->level.objs[i];
        if (o->type >= 1 && o->type <= 29 && o->type != 3 && o->type != 4) enemies_add_trigger(&g->enemies, o);
    }
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

void game_update(Game *g, float dt)
{
    input_update(&g->in);
    Player *p = &g->player; Character *c = &p->ch;
    /* order as in GameLevel::update: controls -> (spawner, enemies) -> physics -> bullets -> camera */
    if (c->state == CS_DEAD) { c->coll = c->body.coll; player_death_update(p, dt, g->level.height, &g->cam_x, g->sw); }
    else { character_sync_ground(c); player_death_update(p, dt, g->level.height, &g->cam_x, g->sw); player_control(p, &g->in, dt); }
    player_try_fire(p, &g->player_bullets, &g->effects, g->player_layer);
    player_check_enemy_bullets(p, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh);
    g->world.world_min_x = g->cam_x;   /* GameLevel::update: physics world min = camera left edge */
    enemies_update(&g->enemies, p, &g->level, &g->world, &g->player_bullets, &g->enemy_bullets, &g->effects, g->cam_x, g->sw, g->sh, dt);
    player_resolve(c, dt);
    physics_step(&g->world, &g->level, &c->body, dt);
    bullets_update(&g->player_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    bullets_update(&g->enemy_bullets, &g->level, &g->effects, dt, g->cam_x, g->cam_y, g->sw, g->sh);
    character_animate(c, dt);
    effects_update(&g->effects, dt);
    player_frame_end(p, dt);
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "st=%d aim=%d face=%d anim=%d frame=%d flags=%x coll=%x pos=%.1f,%.1f v=%.1f,%.1f in=%d%d%d%d%d%d%d\n",
        c->state, c->aim, c->facing, c->anim, c->frame, c->flags, c->coll, c->body.x, c->body.y, c->body.vx, c->body.vy,
        g->in.state[0], g->in.state[1], g->in.state[2], g->in.state[3], g->in.state[4], g->in.state[5], g->in.state[6]);

    if (g->free_cam) {
        float sp = (g->key[SDL_SCANCODE_LSHIFT] ? 600.f : 200.f) * dt;
        if (g->key[SDL_SCANCODE_RIGHT]) g->cam_x += sp;
        if (g->key[SDL_SCANCODE_LEFT]) g->cam_x -= sp;
    } else {
        /* camera follows the player horizontally (FUN_0040c460), max 4 px/frame catch-up */
        float target = c->body.x;
        float half = g->sw * 0.5f;
        float camc = g->cam_x + half;
        float d = target - camc;
        if (fabsf(d) > 4.0f * 2) camc += (d > 0 ? 1 : -1) * 4.0f * 2; else camc = target;
        if (camc < half) camc = half;
        g->cam_x = camc - half;
    }
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

void game_draw(Game *g)
{
    Level *L = &g->level;
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
    hud_draw(g->ren, 0, 1, g->player.lives, g->player.hp, 0);
    if (g->debug_collision) draw_collision(g);
}
