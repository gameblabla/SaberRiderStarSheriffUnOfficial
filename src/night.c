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

/* the level-1 boss's samples and music (enemies.c update_boss / update_boss_rider) */
enum {
    SFX_ENGINE = 0x13,      /* boot / every pass */
    SFX_GUN = 0x10,         /* the ship's gun */
    SFX_PILOT_GUN = 0x12,   /* the rider's gun */
    SFX_BLAST = 0x11,       /* wreck explosions */
    SFX_DOWN = 0x15,        /* the wreck's big bang */
    MUSIC_BOSS = 8,
};

/* art geometry (assets/hyperjumper): side 130x108 drawn facing left, front 201x140 */
#define SIDE_W 130
#define SIDE_H 108
#define FRONT_W 201
#define FRONT_H 140
#define SIDE_MUZZLE_X 70     /* hyperjumper_frame1_shooting: the pilot's gun (unflipped) */
#define SIDE_MUZZLE_Y 54
#define FRONT_MUZZLE_X 93    /* hyperjumper_firing_sprite */
#define FRONT_MUZZLE_Y 45

typedef struct { float x0, y0, x1, y1; } Box;
/* sprite-local boxes (from the art's outline): the hull's hump, the hull, the keel hurt on
 * contact; the pilot and the aerial only take shots */
static const Box SIDE_HULL[] = { { 57, 44, 116, 56 }, { 14, 56, 122, 78 }, { 40, 78, 101, 98 } };
static const Box SIDE_PILOT = { 76, 24, 108, 44 };
static const Box FRONT_HULL[] = { { 6, 58, 195, 98 }, { 82, 28, 118, 124 } };

#define HOVER_Y 66.0f        /* side hold: the keel clears a standing hero, a jump reaches it */
#define FRONT_Y 56.0f
#define DEATH_TIME 3.8f      /* kill(E, e, 0xed8) */

static int rnd(int n) { return n > 0 ? rand() % n : 0; }

static Sprite *load_sprite(const char *name, uint32_t id)
{
    const char *path = asset_path(name);
    if (!path) { fprintf(stderr, "stage3: missing %s\n", name); return NULL; }
    int w = 0, h = 0;
    uint32_t *px = png_load_rgba(path, &w, &h);
    if (!px) { fprintf(stderr, "stage3: can't decode %s\n", name); return NULL; }
    Sprite *s = sprite_from_rgba(id, px, w, h, 1);
    free(px);
    return s;
}

void night_dispose(Night *n)
{
    stage3_world_free(&n->world);
}

bool night_init(Night *n, Level *L, int difficulty)
{
    memset(n, 0, sizeof *n);
    if (!stage3_world_build(L, &n->world)) { fprintf(stderr, "stage3: can't build the route\n"); return false; }
    n->difficulty = difficulty;
    n->start_x = 100.0f; n->start_y = 155.0f;
    n->far_layer = n->mid_layer = n->play_layer = -1;
    for (int i = 0; i < L->nlayers; i++) {
        if (!strcmp(L->layers[i].name, "Small Hyperjmpr")) n->far_layer = i;
        else if (!strcmp(L->layers[i].name, "MidBGHyperjpr")) n->mid_layer = i;
        else if (!strcmp(L->layers[i].name, "PlayerSprites")) n->play_layer = i;
    }
    n->sky = load_sprite("stage3/native/stage3_night_sky.png", 0x48330009);
    n->moon = load_sprite("stage3/native/stage3_red_moon.png", 0x4833000A);
    n->side_normal = load_sprite("hyperjumper/side_normal.png", 0x48330001);
    n->side_boost = load_sprite("hyperjumper/side_boost.png", 0x48330002);
    n->side_fire1 = load_sprite("hyperjumper/side_fire1.png", 0x48330003);
    n->side_fire2 = load_sprite("hyperjumper/side_fire2.png", 0x48330004);
    n->front_idle = load_sprite("hyperjumper/front_idle.png", 0x48330005);
    n->front_fire = load_sprite("hyperjumper/front_fire.png", 0x48330006);
    n->projectile_diagonal = load_sprite("hyperjumper/projectile_diagonal.png", 0x48330007);
    n->projectile_front = load_sprite("hyperjumper/projectile_front.png", 0x48330008);
    n->hp_max = n->hp = SDL_getenv("SABER_BOSSHP") ? atoi(SDL_getenv("SABER_BOSSHP"))   /* debug: SABER_BOSSHP=n */
                      : difficulty == 0 ? 48 : difficulty == 1 ? 60 : 72;
    n->state = HJ_DORMANT;
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "stage3: route %.0f px, boss hp %d\n", L->width, n->hp);
    return true;
}

/* ---- geometry ---- */
static bool front_pose(const Night *n)
{
    return n->state == HJ_FRONT_IN || n->state == HJ_FRONT_FIRE || n->state == HJ_FRONT_OUT ||
           ((n->state == HJ_DYING || n->state == HJ_DONE) && n->death_front);
}

static Box world_box(const Night *n, Box b)
{
    if (front_pose(n)) {
        float l = floorf(n->bx) - FRONT_W / 2, t = floorf(n->by) - FRONT_H / 2;
        return (Box){ l + b.x0, t + b.y0, l + b.x1, t + b.y1 };
    }
    float l = floorf(n->bx) - SIDE_W / 2, t = floorf(n->by) - SIDE_H / 2;
    if (n->dir > 0) { float x0 = SIDE_W - b.x1, x1 = SIDE_W - b.x0; b.x0 = x0; b.x1 = x1; }
    return (Box){ l + b.x0, t + b.y0, l + b.x1, t + b.y1 };
}

static bool fighting(const Night *n)
{
    return n->state >= HJ_SIDE_IN && n->state <= HJ_FRONT_OUT;
}

static bool phase2(const Night *n) { return n->hp * 2 <= n->hp_max; }

static void player_box(const Player *pl, Box *out)
{
    const Character *c = &pl->ch;
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float cx = c->body.x + h->ox, cy = c->body.y + h->oy;
    *out = (Box){ cx - h->hw, cy - h->hh, cx + h->hw, cy + h->hh };
}

static bool overlap(Box a, Box b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

static void hurt_player(Player *pl, int dir)
{
    Character *c = &pl->ch;
    if (c->state == CS_DEAD || (c->flags & CF_HIT)) return;
    player_damage(pl, dir, 1);
}

/* ---- attacks ---- */
static void spawn_shot(Night *n, float x, float y, float vx, float vy, bool front)
{
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        HyperjumperShot *s = &n->shots[i];
        if (s->alive) continue;
        *s = (HyperjumperShot){ true, front, vx > 0, x, y, vx, vy };
        return;
    }
}

static void fire_side(Night *n)
{
    /* the pilot's gun, down-left as drawn / down-right mirrored, at the level-1 boss laser speed */
    float l = floorf(n->bx) - SIDE_W / 2, t = floorf(n->by) - SIDE_H / 2;
    float mx = n->dir > 0 ? l + SIDE_W - SIDE_MUZZLE_X : l + SIDE_MUZZLE_X, my = t + SIDE_MUZZLE_Y;
    float v = 333.3333f * 0.70710678f;
    spawn_shot(n, mx, my, n->dir * v, v, false);
    n->fire_anim = 0.16f;
    sfx_play(SFX_GUN, 0);
}

static void fire_front(Night *n)
{
    float l = floorf(n->bx) - FRONT_W / 2, t = floorf(n->by) - FRONT_H / 2;
    spawn_shot(n, l + FRONT_MUZZLE_X, t + FRONT_MUZZLE_Y, 0, 300.0f, true);
    n->fire_anim = 0.09f;
    sfx_play(SFX_PILOT_GUN, 0);
}

static void set_state(Night *n, HyperState s)
{
    n->state = s; n->st = 0;
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "stage3: hyperjumper state %d at %.0f,%.0f hp %d\n", s, n->bx, n->by, n->hp);
}

static void begin_side(Night *n, int dir)
{
    set_state(n, HJ_SIDE_IN);
    n->dir = dir;
    n->bx = dir < 0 ? n->arena_x + n->sw + SIDE_W * 0.5f + 16 : n->arena_x - SIDE_W * 0.5f - 16;
    n->by = HOVER_Y;
    n->speed = 240.0f;
    sfx_play(SFX_ENGINE, 0);
}

static float low_pass_y(void)
{
    /* The hero's hurtboxes sit on the feet: 46 px standing, 34 jumping, 24 crouched, 12 sliding.
     * The keel passes 3 px over a slide; the hull's top is 69 px off the ground, under the
     * 87 px jump, so a jump carries the hero over it too. */
    return 208.0f - 12.0f - 3.0f - (98 - SIDE_H / 2);
}

static void begin_low(Night *n, int dir)
{
    set_state(n, HJ_LOW_WARN);
    n->dir = dir;
    n->by = low_pass_y();
    n->bx = dir > 0 ? n->arena_x - SIDE_W * 0.5f - 8 : n->arena_x + n->sw + SIDE_W * 0.5f + 8;
    sfx_play(SFX_ENGINE, 0);
}

static void begin_front(Night *n, const Player *pl)
{
    set_state(n, HJ_FRONT_IN);
    float lo = n->arena_x + FRONT_W * 0.5f - 12, hi = n->arena_x + n->sw - FRONT_W * 0.5f + 12;
    float px = pl->ch.body.x;
    n->bx = px < lo ? lo : px > hi ? hi : px;
    n->by = -FRONT_H * 0.5f - 8;
    n->burst = 0;
    n->shot_t = 0.55f;
    sfx_play(SFX_ENGINE, 0);
}

static void begin_death(Night *n)
{
    n->death_front = front_pose(n);   /* the wreck falls in the pose it was hit in */
    set_state(n, HJ_DYING);
    n->death_vy = 0; n->death_phase = 1;
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) n->shots[i].alive = false;
}

static void hit_boss(Night *n, Effects *fx, float x, float y)
{
    if (--n->hp <= 0) { n->hp = 0; begin_death(n); return; }
    n->hit_flash = 0.08f;
    AnimDef a = { 0, 4, 8, 4, 0.03f, 0 };
    effects_spawn(fx, 0x8623249C, n->play_layer, &a, x, y, 8, 8, 0);
}

static void update_shots(Night *n, Player *pl, Effects *fx, float dt)
{
    Box pb; player_box(pl, &pb);
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        HyperjumperShot *s = &n->shots[i];
        if (!s->alive) continue;
        s->x += s->vx * dt; s->y += s->vy * dt;
        float rx = s->front ? 3.0f : 4.0f, ry = s->front ? 6.0f : 4.0f;
        Box sb = { s->x - rx, s->y - ry, s->x + rx, s->y + ry };
        if (overlap(sb, pb) && pl->ch.state != CS_DEAD && !(pl->ch.flags & CF_HIT)) {
            hurt_player(pl, s->front ? AIM_D : s->vx < 0 ? AIM_DL : AIM_DR);
            s->alive = false;
            continue;
        }
        if (s->y >= 206.0f) {   /* splashes on the ground */
            AnimDef a = { 0, 0, 4, 0, 0.03f, 0 };
            effects_spawn(fx, 0x8623249C, n->play_layer, &a, s->x, 204.0f, 8, 8, 0);
            s->alive = false;
            continue;
        }
        if (s->x < n->arena_x - 32 || s->x > n->arena_x + n->sw + 32) s->alive = false;
    }
}

static void boss_collisions(Night *n, Player *pl, Bullets *pb, Effects *fx)
{
    bool front = front_pose(n);
    const Box *hull = front ? FRONT_HULL : SIDE_HULL;
    int nh = front ? 2 : 3;
    Box boxes[4];
    for (int k = 0; k < nh; k++) boxes[k] = world_box(n, hull[k]);
    int nb = nh;
    if (!front) boxes[nb++] = world_box(n, SIDE_PILOT);

    /* the hero's shots: the whole ship takes hits while it is on screen */
    for (int i = 0; i < pb->n; ) {
        Bullet *b = &pb->b[i];
        bool on_screen = b->x > n->arena_x + 2 && b->x < n->arena_x + n->sw - 2;
        bool hit = false;
        for (int k = 0; k < nb && on_screen && !hit; k++)
            hit = b->x > boxes[k].x0 - 3 && b->x < boxes[k].x1 + 3 && b->y > boxes[k].y0 - 3 && b->y < boxes[k].y1 + 3;
        if (!hit) { i++; continue; }
        float x = b->x, y = b->y;
        pb->b[i] = pb->b[--pb->n];
        hit_boss(n, fx, x, y);
        if (n->state == HJ_DYING) return;
    }

    /* ramming: hull and keel (the pilot and the aerial don't hurt) */
    Box hb; player_box(pl, &hb);
    for (int k = 0; k < nh; k++)
        if (overlap(boxes[k], hb)) { hurt_player(pl, n->bx > (hb.x0 + hb.x1) * 0.5f ? AIM_L : AIM_R); break; }
}

void night_update(Night *n, Player *pl, Bullets *pb, Effects *fx, const Level *L,
                  float cam_x, int sw, float dt, bool live)
{
    if (n->hit_flash > 0) n->hit_flash -= dt;
    if (n->fire_anim > 0) n->fire_anim -= dt;
    if (!live) return;
    n->st += dt;
    if (n->state != HJ_DORMANT && n->state != HJ_DYING && n->state != HJ_DONE) update_shots(n, pl, fx, dt);
    const float right = n->arena_x + n->sw;
    bool p2 = phase2(n);
    switch (n->state) {
    case HJ_DORMANT:
        /* the route's end: the camera has stopped on the open ground and the hero is out on it */
        if (pl->ch.state != CS_DEAD && cam_x >= L->width - sw - 0.5f && pl->ch.body.x >= L->width - sw * 0.5f - 8.0f) {
            n->boss_started = true;
            n->arena_x = L->width - sw; n->sw = sw;
            music_play(MUSIC_BOSS, true); sfx_play(SFX_ENGINE, 0);   /* boss phase 0: FUN_00412750 */
            set_state(n, HJ_FAR_PASS);
            n->dir = -1; n->bx = n->arena_x + sw + 40; n->by = 58;
            if (SDL_getenv("SABER_TRACE")) {
                const Character *c = &pl->ch;
                for (int a = 0; a < CHAR_MAX_ANIMS; a++) if (c->hurt[a].hh > 0)
                    fprintf(stderr, "stage3: hero anim %d hurt oy=%.0f hh=%.0f (top %.0f above the feet)\n", a, c->hurt[a].oy, c->hurt[a].hh,
                            c->body.oy + c->body.hy - (c->hurt[a].oy - c->hurt[a].hh));
            }
        }
        break;
    case HJ_FAR_PASS:     /* far behind the mesas, right to left (level 1: the far layer pass) */
        n->bx -= 330.0f * dt;
        if (n->bx < n->arena_x - 40) set_state(n, HJ_FAR_GAP);
        break;
    case HJ_FAR_GAP:
        if (n->st >= 1.3f) { set_state(n, HJ_MID_PASS); n->dir = 1; n->bx = n->arena_x - 60; n->by = 44; sfx_play(SFX_ENGINE, 0); }
        break;
    case HJ_MID_PASS:     /* nearer, left to right, behind the play plane's rocks */
        n->bx += 390.0f * dt;
        n->by = 44 + sinf(n->st * 4.0f) * 3.0f;
        if (n->bx > right + 60) set_state(n, HJ_MID_GAP);
        break;
    case HJ_MID_GAP:
        if (n->st >= 1.0f) begin_side(n, -1);
        break;
    case HJ_SIDE_IN: {    /* flies in and settles over one half of the arena (level 1: centre +128 / -160) */
        float target = n->arena_x + n->sw * 0.5f - n->dir * 96.0f;
        float d = target - n->bx;
        float sp = fabsf(d) * 2.8f; if (sp > 260.0f) sp = 260.0f; if (sp < 40.0f) sp = 40.0f;
        if (fabsf(d) <= sp * dt) { n->bx = target; set_state(n, HJ_SIDE_HOLD); n->shot_t = 0.3f; }
        else n->bx += (d > 0 ? 1 : -1) * sp * dt;
        n->by = HOVER_Y + sinf(n->st * 3.0f) * 2.0f;
        break; }
    case HJ_SIDE_HOLD:    /* hovers, the pilot sprays the ground diagonally */
        n->by = HOVER_Y + sinf(n->st * 3.0f) * 2.0f;
        n->shot_t -= dt;
        if (n->shot_t <= 0) { fire_side(n); n->shot_t = p2 ? 0.26f : 0.34f; }
        if (n->st >= (p2 ? 2.1f : 1.7f)) { set_state(n, HJ_SIDE_OUT); n->speed = 60.0f; }
        break;
    case HJ_SIDE_OUT:     /* boosts away the way it faces */
        n->speed += 520.0f * dt; if (n->speed > 400.0f) n->speed = 400.0f;
        n->bx += n->dir * n->speed * dt;
        n->by = HOVER_Y - n->st * 20.0f;
        if (n->dir < 0 ? n->bx < n->arena_x - SIDE_W * 0.5f - 8 : n->bx > right + SIDE_W * 0.5f + 8) begin_low(n, -n->dir);
        break;
    case HJ_LOW_WARN: {   /* the nose shows at the edge, engine howling, before the ground run */
        float peek = n->dir > 0 ? n->arena_x - SIDE_W * 0.5f + 30 : right + SIDE_W * 0.5f - 30;
        n->bx += (peek - n->bx) * (dt * 8.0f > 1 ? 1 : dt * 8.0f);
        if (n->st >= (n->difficulty == 2 ? 0.6f : 0.8f)) { set_state(n, HJ_LOW_PASS); n->speed = p2 ? 330.0f : 290.0f; }
        break; }
    case HJ_LOW_PASS:     /* jump over it or slide under it */
        n->bx += n->dir * n->speed * dt;
        if (n->dir > 0 ? n->bx > right + SIDE_W * 0.5f + 8 : n->bx < n->arena_x - SIDE_W * 0.5f - 8) begin_front(n, pl);
        break;
    case HJ_FRONT_IN:     /* drops in facing the hero */
        n->by += 150.0f * dt;
        if (n->by >= FRONT_Y) { n->by = FRONT_Y; set_state(n, HJ_FRONT_FIRE); }
        break;
    case HJ_FRONT_FIRE: { /* drifts after the hero, bursts of bolts straight down from the pilot's gun */
        float lo = n->arena_x + FRONT_W * 0.5f - 12, hi = right - FRONT_W * 0.5f + 12;
        float px = pl->ch.body.x; if (px < lo) px = lo; if (px > hi) px = hi;
        float drift = (p2 ? 70.0f : 50.0f) * dt, d = px - n->bx;
        n->bx += fabsf(d) <= drift ? d : (d > 0 ? drift : -drift);
        n->by = FRONT_Y + sinf(n->st * 2.5f) * 3.0f;
        n->shot_t -= dt;
        if (n->shot_t <= 0) {
            fire_front(n);
            if (++n->burst < 3) n->shot_t = 0.16f;
            else { n->burst = 0; n->shot_t = p2 ? 0.75f : 0.95f; }
        }
        if (n->st >= (p2 ? 5.0f : 4.2f)) set_state(n, HJ_FRONT_OUT);
        break; }
    case HJ_FRONT_OUT:
        n->by -= 190.0f * dt;
        if (n->by < -FRONT_H * 0.5f - 8) { n->cycle++; begin_side(n, (n->cycle & 1) ? 1 : -1); }
        break;
    case HJ_DYING:        /* level-1 boss state 9: the wreck drops, jitters and burns for 0xed8 ms */
        n->death_vy += 480.0f * dt;
        n->by += n->death_vy * dt;
        n->bx += (float)rnd(180) / 60.0f - 153.0f / 60.0f;
        if (rnd(10) >= 9) {
            AnimDef a = { 0, 0, 11, 11, 0.025f, 0 };
            effects_spawn(fx, 0x9C861FF3, n->play_layer, &a, n->bx - 40 + rnd(80), n->by - 30 + rnd(60), 32, 32, 0);
            if (n->death_phase < 0x40 && (n->death_phase & 1)) sfx_play(SFX_BLAST, 0);
        }
        if (n->death_phase == 8) sfx_play(SFX_DOWN, 0);
        n->death_phase++;
        if (n->st >= DEATH_TIME) { set_state(n, HJ_DONE); n->clear_ready = true; }
        break;
    case HJ_DONE:
        break;
    }
    if (fighting(n) && pl->ch.state != CS_DEAD) boss_collisions(n, pl, pb, fx);
}

/* ---- drawing ---- */
bool night_layer_tint(const Night *n, const Layer *ly, uint8_t *r, uint8_t *g, uint8_t *b)
{
    (void)n;
    static const struct { const char *name; uint8_t r, g, b; } TINT[] = {
        { "FarMountains", 58, 62, 118 }, { "Mountains", 66, 70, 130 }, { "NearMountains", 76, 80, 142 },
        { "MidBG", 88, 90, 150 }, { "Cars MidBG", 88, 90, 150 },
        { "Playfield", 112, 110, 168 }, { "Platforms", 116, 114, 172 }, { "Cars", 116, 114, 172 },
        { "ForegroundStuff", 100, 98, 156 }, { "ForegroundStuf2", 52, 52, 92 },
    };
    if (!strcmp(ly->name, "SkyBG")) return false;   /* the night sky replaces it */
    *r = 100; *g = 100; *b = 160;
    for (size_t i = 0; i < sizeof TINT / sizeof TINT[0]; i++)
        if (!strcmp(ly->name, TINT[i].name)) { *r = TINT[i].r; *g = TINT[i].g; *b = TINT[i].b; }
    return true;
}

void night_draw_background(Night *n, float cam_x, int sw, int sh)
{
    (void)sh;
    if (n->sky) {
        int w = n->sky->w;
        float x = -fmodf(floorf(cam_x * 0.05f), (float)w);
        for (; x < sw; x += w) sprite_draw(n->sky, 0, x, 0, false);
    }
    /* the moon hangs behind every tile layer, drifting a little over the whole route */
    if (n->moon) sprite_draw(n->moon, 0, floorf(sw * 0.70f - cam_x * 0.02f), 22, false);
}

static void draw_sprite(SDL_Renderer *ren, const Sprite *s, float cx, float cy, float scale, bool flip,
                        uint8_t r, uint8_t g, uint8_t b, float angle)
{
    if (!s) return;
    SDL_FRect src = { 0, 0, (float)s->w, (float)s->h };
    float w = roundf(s->w * scale), h = roundf(s->h * scale);
    SDL_FRect dst = { floorf(cx) - floorf(w * 0.5f), floorf(cy) - floorf(h * 0.5f), w, h };
    SDL_SetTextureColorMod(s->tex, r, g, b);
    SDL_RenderTextureRotated(ren, s->tex, &src, &dst, angle, NULL, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureColorMod(s->tex, 255, 255, 255);
}

void night_draw_layer(Night *n, SDL_Renderer *ren, int layer, float cam_x, float cam_y)
{
    float x = n->bx - cam_x, y = n->by - cam_y;
    bool blink = ((int)(n->st * 30.0f)) & 1;
    if (layer == n->far_layer && n->state == HJ_FAR_PASS)
        draw_sprite(ren, blink ? n->side_boost : n->side_normal, x, y, 0.4f, n->dir > 0, 96, 96, 140, 0);
    if (layer == n->mid_layer && n->state == HJ_MID_PASS)
        draw_sprite(ren, blink ? n->side_boost : n->side_normal, x, y, 0.65f, n->dir > 0, 150, 150, 190, 0);
    if (layer != n->play_layer) return;

    if (fighting(n) || n->state == HJ_DYING) {
        const Sprite *s;
        float angle = 0;
        uint8_t r = 255, g = 255, b = 255;
        if (n->hit_flash > 0) { g = 150; b = 150; }
        if (front_pose(n)) s = n->fire_anim > 0 ? n->front_fire : n->front_idle;
        else if (n->fire_anim > 0) s = n->fire_anim > 0.09f ? n->side_fire1 : n->side_fire2;
        else if (n->state == HJ_SIDE_OUT || n->state == HJ_LOW_PASS || (n->state == HJ_LOW_WARN && blink) ||
                 (n->state == HJ_SIDE_IN && n->speed > 120.0f)) s = n->side_boost;
        else s = n->side_normal;
        if (n->state == HJ_DYING) {
            angle = n->st * (n->death_front ? 6.0f : -12.0f);
            uint8_t v = (uint8_t)(255 - 110 * (n->st / DEATH_TIME));
            r = g = b = v;
            if (((int)(n->st * 20)) & 1) { g = (uint8_t)(v * 0.6f); b = (uint8_t)(v * 0.5f); }
        }
        draw_sprite(ren, s, x, y, 1.0f, !front_pose(n) && n->dir > 0, r, g, b, angle);
    }
    for (int i = 0; i < HYPERJUMPER_SHOTS; i++) {
        const HyperjumperShot *s = &n->shots[i];
        if (!s->alive) continue;
        const Sprite *spr = s->front ? n->projectile_front : n->projectile_diagonal;
        draw_sprite(ren, spr, s->x - cam_x, s->y - cam_y, 1.0f, s->flip, 255, 255, 255, 0);
    }
}

void night_draw_hud(Night *n, SDL_Renderer *ren, int sw, int sh)
{
    (void)sh;
    if (!n->boss_started || n->state < HJ_SIDE_IN || n->state >= HJ_DONE) return;
    Font *f = font_get(0x12072E60);
    float bw = 96, x = sw - bw - 10, y = 20;
    if (f) font_draw(f, "HYPERJUMPER", (int)x, 8, 220, 226, 245);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 10, 12, 30, 200);
    SDL_FRect bg = { x - 1, y - 1, bw + 2, 7 };
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 230, 50, 50, 255);
    SDL_FRect fg = { x, y, bw * (float)n->hp / (float)n->hp_max, 5 };
    SDL_RenderFillRect(ren, &fg);
}
