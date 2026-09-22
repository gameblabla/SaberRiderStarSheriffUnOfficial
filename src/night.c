#include "night.h"
#include "assets.h"
#include "audio.h"
#include "enemies.h"
#include "font.h"
#include "gfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define HYPER_SIDE_SPEED 300.0f
#define HYPER_BOOST_SPEED 520.0f

typedef struct { float x0, x1; int row; } Platform;

/* A broken bridge / landing-pad rhythm, intentionally unlike the long flat
 * frontier-town run. World units are pixels; the source collision grid is 8px. */
static const Platform PLATFORMS[] = {
    { 560, 1040, 22 }, { 1320, 1810, 16 }, { 2050, 2440, 22 },
    { 2600, 3230, 10 }, { 3420, 3880, 20 }, { 4070, 4520, 14 },
    { 4760, 5280, 22 }, { 5500, 5940, 12 },
};
static const struct { float x0, x1; } PITS[] = {
    { 1060, 1300 }, { 1840, 2030 }, { 3270, 3400 },
    { 3890, 4060 }, { 5300, 5480 },
};

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static bool in_range(float x, float x0, float x1)
{
    return x >= x0 && x < x1;
}

static Sprite *load_sprite(const char *name, uint32_t id)
{
    const char *path = asset_path(name);
    if (!path) {
        fprintf(stderr, "stage3: missing %s\n", name);
        return NULL;
    }
    int w = 0, h = 0;
    uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) return NULL;
    Sprite *s = sprite_from_rgba(id, px, w, h, 1);
    free(px);
    return s;
}

static void load_boss_art(Night *n)
{
    n->side_normal = load_sprite("hyperjumper/side_normal.png", 0x48330001);
    n->side_boost = load_sprite("hyperjumper/side_boost.png", 0x48330002);
    n->side_fire1 = load_sprite("hyperjumper/side_fire1.png", 0x48330003);
    n->side_fire2 = load_sprite("hyperjumper/side_fire2.png", 0x48330004);
    n->front_idle = load_sprite("hyperjumper/front_idle.png", 0x48330005);
    n->front_fire = load_sprite("hyperjumper/front_fire.png", 0x48330006);
    n->projectile_diagonal = load_sprite("hyperjumper/projectile_diagonal.png", 0x48330007);
    n->projectile_front = load_sprite("hyperjumper/projectile_front.png", 0x48330008);
}

static void load_background(Night *n)
{
    n->sky = load_sprite("stage3_night_sky.png", 0x48330009);
    n->moon = load_sprite("stage3_red_moon.png", 0x4833000A);
}

static void load_static_art(Night *n)
{
    static const char *const names[STAGE3_STATIC_ART] = {
        "stage3_static_mesa.png", "stage3_static_cactus.png", "stage3_static_scrub.png",
        "stage3_static_fence.png", "stage3_static_wreck.png", "stage3_static_pad.png",
    };
    for (int i = 0; i < STAGE3_STATIC_ART; i++) n->static_art[i] = load_sprite(names[i], 0x48330020u + (uint32_t)i);
}

static void build_arena_collision(Night *n, Level *L)
{
    size_t bytes = (size_t)L->cols * (size_t)L->rows;
    n->arena_collision = calloc(bytes, 1);
    if (!n->arena_collision) return;

    int ground_row = 26;
    int max_col = (int)ceilf(n->arena_width / L->cellw);
    if (max_col > L->cols) max_col = L->cols;
    for (int c = 0; c < max_col; c++) {
        float x = c * L->cellw + L->cellw * 0.5f;
        bool pit = false;
        for (size_t i = 0; i < sizeof PITS / sizeof PITS[0]; i++) if (in_range(x, PITS[i].x0, PITS[i].x1)) pit = true;
        if (!pit) n->arena_collision[ground_row * L->cols + c] = 4;
    }
    for (size_t i = 0; i < sizeof PLATFORMS / sizeof PLATFORMS[0]; i++) {
        const Platform *p = &PLATFORMS[i];
        int c0 = (int)floorf(p->x0 / L->cellw), c1 = (int)ceilf(p->x1 / L->cellw);
        for (int c = c0; c < c1 && c < L->cols; c++) if (c >= 0) n->arena_collision[p->row * L->cols + c] = 4;
    }
    L->collision = n->arena_collision;
    L->width = n->arena_width;
    L->height = 256.0f;
}

static bool rearrange_layer(const char *name)
{
    /* Leave only the repeating sky sequence in order. Every other layer gets
     * the same level-1 tile bank, but its eight source strips are presented in
     * a different sequence for the boss pass. */
    return strcmp(name, "SkyBG") != 0;
}

static void rearrange_tilemaps(Night *n, Level *L)
{
    /* Keep the town-heavy strips out of the route. Source bands 0 and 1 carry
     * the level-1 desert/rock language; repeating that palette gives stage 3
     * an open badlands read instead of a row of storefronts. */
    static const int ORDER[8] = { 0, 1, 1, 1, 0, 1, 1, 1 };
    for (int i = 0; i < L->nlayers; i++) {
        Layer *layer = &L->layers[i];
        if (!layer->is_tilemap || !layer->map) continue;
        TileMap *m = layer->map;
        int mi = (int)(m - L->maps);
        if (mi < 0 || mi >= LVL_MAX_LAYERS) continue;
        size_t count = (size_t)m->w * (size_t)m->h;
        uint32_t *cells = malloc(count * sizeof *cells);
        if (!cells) continue;
        n->map_cells[mi] = cells;
        if (!rearrange_layer(layer->name)) {
            memcpy(cells, m->cells, count * sizeof *cells);
        } else {
            int span = m->w / 8; if (span < 1) span = 1;
            for (int r = 0; r < m->h; r++) for (int c = 0; c < m->w; c++) {
                int band = (c / span) & 7;
                int src = ORDER[band] * span + (c % span);
                if (src >= m->w) src %= m->w;
                cells[r * m->w + c] = m->cells[r * m->w + src];
            }
        }
        m->cells = cells;
    }
}

void night_dispose(Night *n)
{
    if (!n) return;
    for (int i = 0; i < LVL_MAX_LAYERS; i++) { free(n->map_cells[i]); n->map_cells[i] = NULL; }
    free(n->arena_collision);
    n->arena_collision = NULL;
}

void night_init(Night *n, Level *L, int difficulty)
{
    memset(n, 0, sizeof *n);
    n->active = true;
    n->difficulty = difficulty;
    n->arena_width = 6200.0f;
    n->start_x = 150.0f;
    n->start_y = 170.0f;
    build_arena_collision(n, L);
    rearrange_tilemaps(n, L);
    load_background(n);
    load_static_art(n);
    load_boss_art(n);
    n->boss_alive = true;
    n->boss_state = HYPER_READY;
    n->boss_dir = 1;
    n->state_t = 0.75f;
    n->shot_t = 0.35f;
    n->hp_max = n->hp = difficulty == 0 ? 18 : difficulty == 1 ? 24 : 30;
    n->boss_x = -150.0f;
    n->boss_y = 120.0f;
    snprintf(n->msg, sizeof n->msg, "HYPERJUMPER INBOUND");
    n->msg_t = 2.2f;
    fprintf(stderr, "stage3: Hyperjumper Pass %.0fx%.0f hp=%d\n", n->arena_width, L->height, n->hp);
}

static void clear_shot(HyperjumperShot *s)
{
    s->alive = false;
}

static void spawn_shot(Night *n, float x, float y, float vx, float vy, bool front, float angle)
{
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        HyperjumperShot *s = &n->shots[i];
        if (s->alive) continue;
        *s = (HyperjumperShot){ true, x, y, vx, vy, 4.0f, angle, front };
        return;
    }
}

static void side_fire(Night *n, Player *pl)
{
    float px = pl->ch.body.x + pl->ch.body.ox;
    float py = pl->ch.body.y + pl->ch.body.oy;
    float sx = n->boss_x + n->boss_dir * 8.0f;
    float sy = n->boss_y + 6.0f;
    float dx = px - sx, dy = py - sy, d = sqrtf(dx * dx + dy * dy);
    if (d < 1.0f) d = 1.0f;
    float speed = 235.0f;
    spawn_shot(n, sx, sy, dx / d * speed, dy / d * speed, false, atan2f(dy, dx) * 180.0f / PI + 90.0f);
    sfx_play(7, 0);
}

static void front_fire(Night *n)
{
    float y = n->boss_y + 58.0f;
    static const float FAN[] = { -105.0f, -52.0f, 0.0f, 52.0f, 105.0f };
    for (size_t i = 0; i < sizeof FAN / sizeof FAN[0]; i++) {
        float vx = FAN[i] * 0.56f;
        float vy = 235.0f - fabsf(FAN[i]) * 0.14f;
        spawn_shot(n, n->boss_x + FAN[i] * 0.42f, y, vx, vy, true, atan2f(vx, vy) * 180.0f / PI);
    }
    sfx_play(7, 0);
}

static void begin_sweep(Night *n, float cam_x, int sw, int dir)
{
    static const float ALTITUDE[] = { 166.0f, 103.0f, 141.0f, 82.0f };
    n->boss_state = HYPER_SWEEP;
    n->boss_dir = dir < 0 ? -1 : 1;
    n->boss_x = n->boss_dir > 0 ? cam_x - 150.0f : cam_x + sw + 150.0f;
    n->boss_y = ALTITUDE[n->passes % (int)(sizeof ALTITUDE / sizeof ALTITUDE[0])];
    n->passes++;
    n->boost_t = 0.55f;
    n->shot_t = 0.28f;
    n->state_t = 0;
    sfx_play(13, 0);
}

static bool crouched(const Player *pl)
{
    const Character *c = &pl->ch;
    return c->state == CS_CROUCH || c->state == CS_SLIDE || (c->flags & CF_CROUCH) != 0;
}

static void boss_damage(Night *n, Effects *fx, float x, float y, int layer)
{
    n->hp--;
    n->hit_flash = 0.12f;
    sfx_play(14, 0);
    AnimDef a = { 0, 0, 5, 0, 0.035f, 0 };
    effects_spawn(fx, 0x8623249C, layer, &a, x, y, 8, 8, 0);
    if (n->hp > 0) return;
    n->boss_alive = false;
    n->boss_state = HYPER_DEATH;
    n->death_t = 2.0f;
    n->white = 0.7f;
    n->msg_t = 2.5f;
    snprintf(n->msg, sizeof n->msg, "HYPERJUMPER DOWN");
    sfx_play(5, 0);
    sfx_play(6, 3);
}

static void update_shots(Night *n, Player *pl, float cam_x, int sw, int sh, float dt)
{
    Character *c = &pl->ch;
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float px = c->body.x + h->ox, py = c->body.y + h->oy;
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        HyperjumperShot *s = &n->shots[i];
        if (!s->alive) continue;
        s->x += s->vx * dt;
        s->y += s->vy * dt;
        s->ttl -= dt;
        float r = s->front ? 5.0f : 6.0f;
        if (c->state != CS_DEAD && !(c->flags & CF_HIT) && fabsf(px - s->x) <= h->hw + r && fabsf(py - s->y) <= h->hh + r) {
            player_damage(pl, s->vx < 0 ? 0 : 4, 1);
            clear_shot(s);
            continue;
        }
        float sx = s->x - cam_x, sy = s->y;
        if (s->ttl <= 0 || sx < -30 || sx > sw + 30 || sy < -30 || sy > sh + 30) clear_shot(s);
    }
}

static void boss_hit_test(Night *n, Player *pl, Bullets *pb, Effects *fx, int layer)
{
    float hw = n->boss_state == HYPER_FRONT ? 85.0f : 66.0f;
    float hh = n->boss_state == HYPER_FRONT ? 57.0f : 48.0f;
    for (int i = 0; i < pb->n; ) {
        Bullet *b = &pb->b[i];
        if (b->kind != BK_PLAYER || fabsf(b->x - n->boss_x) > hw || fabsf(b->y - n->boss_y) > hh) { i++; continue; }
        float hx = b->x, hy = b->y;
        pb->b[i] = pb->b[--pb->n];
        boss_damage(n, fx, hx, hy, layer);
        if (!n->boss_alive) return;
    }
    Character *c = &pl->ch;
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float px = c->body.x + h->ox, py = c->body.y + h->oy;
    bool near = fabsf(px - n->boss_x) < hw - 4 && fabsf(py - n->boss_y) < hh - 12;
    if (near && n->body_hit_cd <= 0 && c->state != CS_DEAD) {
        bool low_pass = n->boss_state == HYPER_SWEEP && n->boss_y > 150.0f;
        if (!low_pass || !crouched(pl)) {
            player_damage(pl, n->boss_dir > 0 ? 4 : 0, 1);
            n->body_hit_cd = 0.8f;
        }
    }
}

void night_update(Night *n, Player *pl, Bullets *pb, Effects *fx,
                  float cam_x, int sw, int sh, float dt, bool live)
{
    n->t += dt;
    if (n->msg_t > 0) n->msg_t -= dt;
    if (n->white > 0) n->white -= dt * 1.5f;
    if (n->hit_flash > 0) n->hit_flash -= dt;
    if (n->body_hit_cd > 0) n->body_hit_cd -= dt;
    update_shots(n, pl, cam_x, sw, sh, dt);
    if (!live) return;
    if (!n->boss_alive) {
        if (n->boss_state == HYPER_DEATH) {
            n->death_t -= dt;
            if (n->death_t <= 0) n->clear_ready = true;
        }
        return;
    }
    boss_hit_test(n, pl, pb, fx, 11);
    if (!n->boss_alive) return;

    if (n->boss_state == HYPER_READY) {
        n->state_t -= dt;
        if (n->state_t <= 0) {
            int dir = n->passes & 1 ? -1 : 1;
            begin_sweep(n, cam_x, sw, dir);
        }
    } else if (n->boss_state == HYPER_SWEEP) {
        n->state_t += dt;
        n->boost_t -= dt;
        float speed = n->boost_t > 0 ? HYPER_BOOST_SPEED : HYPER_SIDE_SPEED;
        n->boss_x += n->boss_dir * speed * dt;
        n->boss_y += sinf(n->state_t * 7.0f) * dt * (n->boss_y < 110 ? 8.0f : 4.0f);
        n->shot_t -= dt;
        if (n->shot_t <= 0 && n->boss_x > cam_x - 80 && n->boss_x < cam_x + sw + 80) {
            side_fire(n, pl);
            n->shot_t = n->boost_t > 0 ? 0.60f : 0.42f;
        }
        bool gone = n->boss_dir > 0 ? n->boss_x > cam_x + sw + 160 : n->boss_x < cam_x - 160;
        if (gone) {
            if (n->passes % 3 == 0) {
                n->boss_state = HYPER_FRONT;
                n->boss_x = cam_x + sw * 0.52f;
                n->boss_y = 76.0f;
                n->state_t = 0;
                n->shot_t = 0.45f;
                snprintf(n->msg, sizeof n->msg, "FRONTAL VOLLEY");
                n->msg_t = 1.4f;
            } else {
                n->boss_state = HYPER_READY;
                n->state_t = 0.42f;
            }
        }
    } else if (n->boss_state == HYPER_FRONT) {
        n->state_t += dt;
        n->boss_x = cam_x + sw * 0.52f + sinf(n->state_t * 2.2f) * 28.0f;
        n->boss_y = 76.0f + sinf(n->state_t * 3.0f) * 5.0f;
        n->shot_t -= dt;
        if (n->shot_t <= 0) {
            front_fire(n);
            n->shot_t = n->difficulty == 0 ? 1.05f : n->difficulty == 1 ? 0.86f : 0.68f;
        }
        if (n->state_t > 3.2f) {
            n->boss_state = HYPER_READY;
            n->state_t = 0.35f;
        }
    }
}

void night_draw_boss(Night *n, SDL_Renderer *ren, float cam_x, float cam_y)
{
    float x = n->boss_x - cam_x, y = n->boss_y - cam_y;
    if (n->boss_state == HYPER_DEATH) {
        if (n->death_t <= 0) return;
        float f = 1.0f - clampf(n->death_t / 2.0f, 0, 1);
        if (n->side_normal) sprite_draw_rotated(n->side_normal, 0, x, y + f * 80.0f, 1.0f - f * .25f, f * 68.0f, 255, (uint8_t)(255 * (1 - f * .55f)));
        return;
    }
    if (n->boss_state == HYPER_FRONT) {
        Sprite *s = n->shot_t < 0.20f ? n->front_fire : n->front_idle;
        if (s) sprite_draw_mod(s, 0, floorf(x - s->w * .5f), floorf(y - s->h * .5f), 255, n->hit_flash > 0 ? 240 : 255, n->hit_flash > 0 ? 240 : 255, 255);
        return;
    }
    Sprite *s = n->boost_t > 0 ? n->side_boost : (n->shot_t > 0 && n->shot_t < .14f ? (((int)(n->t * 20) & 1) ? n->side_fire1 : n->side_fire2) : n->side_normal);
    if (!s) return;
    SDL_SetTextureColorMod(s->tex, 255, n->hit_flash > 0 ? 240 : 255, n->hit_flash > 0 ? 240 : 255);
    SDL_FRect src = { 0, 0, (float)s->w, (float)s->h }, dst = { floorf(x - s->w * .5f), floorf(y - s->h * .5f), (float)s->w, (float)s->h };
    SDL_RenderTextureRotated(ren, s->tex, &src, &dst, 0, NULL, n->boss_dir > 0 ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureColorMod(s->tex, 255, 255, 255);
}

void night_draw_background(Night *n, float cam_x, float cam_y, int sw, int sh)
{
    if (!n->sky) return;
    (void)cam_y;
    float scale = (float)sh / n->sky->h;
    float w = n->sky->w * scale;
    if (w <= 0) return;
    float x = -fmodf(cam_x * 0.18f + w * 0.20f, w);
    for (; x < sw; x += w) sprite_draw_scaled(n->sky, 0, floorf(x), 0, w, (float)sh);
    if (n->moon) {
        float mh = sh * 0.78f, mw = n->moon->w * mh / n->moon->h;
        float cx = sw * 0.64f - cam_x * 0.18f, cy = sh * 0.46f;
        sprite_draw_scaled(n->moon, 0, floorf(cx - mw * 0.5f), floorf(cy - mh * 0.5f), mw, mh);
    }
}

typedef struct { int art; float x, w, h; } StaticProp;

static const StaticProp STATIC_PROPS[] = {
    { 0,  720, 170, 164 }, { 1,  430,  64, 150 }, { 2,  830,  86, 105 },
    { 3, 1120, 138, 106 }, { 4, 1660, 170, 106 }, { 1, 1880,  66, 154 },
    { 0, 2220, 160, 158 }, { 5, 2870, 142,  76 }, { 2, 3120,  92, 112 },
    { 3, 3500, 132, 102 }, { 4, 4040, 175, 108 }, { 1, 4320,  66, 154 },
    { 0, 4680, 174, 166 }, { 3, 5070, 138, 106 }, { 5, 5480, 146,  78 },
    { 2, 5750,  92, 112 },
};

void night_draw_static(Night *n, float cam_x, float cam_y, int sw)
{
    for (size_t i = 0; i < sizeof STATIC_PROPS / sizeof STATIC_PROPS[0]; i++) {
        const StaticProp *p = &STATIC_PROPS[i];
        Sprite *s = p->art >= 0 && p->art < STAGE3_STATIC_ART ? n->static_art[p->art] : NULL;
        if (!s || p->x + p->w < cam_x - 32 || p->x > cam_x + sw + 32) continue;
        sprite_draw_scaled(s, 0, floorf(p->x - cam_x), floorf(216.0f - p->h - cam_y), p->w, p->h);
    }
}

void night_draw_projectiles(Night *n, SDL_Renderer *ren, float cam_x, float cam_y)
{
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        const HyperjumperShot *s = &n->shots[i];
        if (!s->alive) continue;
        Sprite *spr = s->front ? n->projectile_front : n->projectile_diagonal;
        if (spr) sprite_draw_rotated(spr, 0, s->x - cam_x, s->y - cam_y, 1.0f, s->angle, 255, 255);
    }
}

void night_draw_banner(Night *n, SDL_Renderer *ren, int sw, int sh)
{
    (void)ren;
    (void)sw;
    (void)sh;
    if (n->boss_alive && n->hp < n->hp_max) {
        Font *small = font_get(0x12072E60);
        char hp[32];
        snprintf(hp, sizeof hp, "HYPERJUMPER %02d/%02d", n->hp, n->hp_max);
        if (small) font_draw(small, hp, 10, 8, 206, 220, 240);
    }
    if (n->msg_t > 0) {
        Font *f = font_get(0x4058897F);
        if (f) {
            float w = (float)font_text_width(f, n->msg), x = sw * .5f - w * .5f;
            font_draw(f, n->msg, x + 1, 102, 18, 17, 12);
            font_draw(f, n->msg, x, 101, 244, 206, 100);
        }
    }
}
