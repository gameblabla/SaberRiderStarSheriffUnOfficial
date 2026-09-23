#include "enemies.h"
#include "pack.h"
#include "audio.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* type -> CRHC (binary table at 0x7c5260), index = type-2 */
static const uint32_t TYPE_CRHC[28] = {
    0x112DF34C, 0x02A38AFB, 0x02A38AFB, 0xD39700C4, 0x112DF34C, 0xD39700C4, 0x112DF34C, 0xD39700C4,
    0x2A02BD4F, 0xFBFAF817, 0x4042CD71, 0x71887ECA, 0xBFDAB70F, 0x1D724DD9, 0x211F5D78, 0x9393E59B,
    0x20C6FAEF, 0xECC992CB, 0x72B53EF8, 0x925534E2, 0x916137ED, 0x906D3698, 0xF5975DCF, 0xF4A55EDE,
    0xF4A25ED9, 0xF3B05E28, 0x0DB9F0E0, 0xD39700C4 };

static uint32_t crhc_for_type(int t) { return (t >= 2 && t <= 29) ? TYPE_CRHC[t - 2] : (t >= 30 && t <= 32) ? 0xD39700C4 : 0x02A38AFB; }   /* 30/31: the shield sniper stands on the sniper's body, 32 the stalker */
static float sh_feet(const Enemy *e);
static int rnd(int n) { return n > 0 ? rand() % n : 0; }   /* FUN_0040cf30(0, n) -> [0,n) */

void enemies_reset(Enemies *E) { memset(E, 0, sizeof *E); E->spawner_enabled = true; E->front_layer = -1; }

void enemies_add_trigger(Enemies *E, const LevelObject *o)   /* FUN_00421070 + FUN_004211b0 */
{
    if (E->ntr >= MAX_TRIGGERS) return;
    Trigger *t = &E->tr[E->ntr++];
    memset(t, 0, sizeof *t);
    t->hx = o->spawn_x * 0.5f; t->hy = o->spawn_y * 0.5f;
    t->cx = o->x + t->hx; t->cy = o->y + t->hy;
    t->type = o->type; t->layer = o->layer;
    t->interval_ms = o->a; t->rand_n = o->c;
    t->timer = o->b * 0.001f + (o->d ? rnd(o->d + 1) * 0.02f : 0);
    t->remaining = o->loops; t->remaining_init = o->loops;
    for (int i = 0; i < o->n_wp && i < 8; i++) { t->wp[i][0] = o->wp[i][0]; t->wp[i][1] = o->wp[i][1]; t->nwp++; }
    if (t->nwp == 0) { t->wp[0][0] = o->x; t->wp[0][1] = o->y; t->nwp = 1; }
    if (SDL_getenv("SABER_TRACE")) { fprintf(stderr, "trigger type=%d zone=%.0f..%.0f x %.0f..%.0f every=%d loops=%d wp=", t->type, t->cx - t->hx, t->cx + t->hx, t->cy - t->hy, t->cy + t->hy, t->interval_ms, t->remaining); for (int i = 0; i < t->nwp; i++) fprintf(stderr, "(%.0f,%.0f)", t->wp[i][0], t->wp[i][1]); fprintf(stderr, "\n"); }
}

static Enemy *alloc_slot(Enemies *E, int cls)   /* FUN_0041f980 */
{
    for (int i = 0; i < MAX_ENEMIES; i++) if (!E->e[i].cls) { memset(&E->e[i], 0, sizeof(Enemy)); E->e[i].cls = (uint16_t)cls; E->e[i].hp = 1; E->e[i].link = -1; E->count++; return &E->e[i]; }
    return NULL;
}

static void kill(Enemies *E, Enemy *e, int ms)   /* FUN_0041faf0 */
{
    (void)E; e->dying = true; e->death_t = ms * 0.001f;
}

/* --- physics probes (FUN_00420be0 / FUN_00420290 / FUN_00420730) --- */
static void snap_to_ground(Enemy *e, const Level *L, const PhysicsWorld *W)
{
    Character *c = &e->ch; Body *b = &c->body;
    if (b->vy < -1.0f || b->vy > 1.0f) return;
    if (c->state != CS_AIR || (c->flags & CF_SPAWN_FALL)) return;
    Body probe = *b; probe.y -= 4.0f; probe.vx = probe.vy = 0;
    for (int i = 0; i < 12; i++) {
        probe.vx = probe.vy = 0;
        physics_step(W, L, &probe, 0.001f);
        if (probe.coll & COLL_DOWN) { b->y = probe.y; c->state = CS_IDLE; return; }
        probe.y += 1.0f;
    }
}

/* FUN_00420290 (facing right) / FUN_00420730 (facing left): spawn probe for the screen-edge spawns. Walk 32 steps
 * ahead; if a wall is hit, try again with a jump; if that hits too, raise the spawn point by 8 px, add 8 steps and
 * retry until the body clears (or y reaches the top). The body is left at the raised position, so an enemy whose
 * spawn point falls inside a crashed car ends up above it and drops onto its roof instead of jittering inside. */
static void face_and_probe(Enemy *e, bool right, const Level *L, const PhysicsWorld *W, float cam_x, int sw)
{
    Character *c = &e->ch; Body *b = &c->body;
    if (right) character_move_right(c, 4); else character_move_left(c, 0);
    if (right ? b->x > cam_x + 16.0f : b->x < cam_x + sw - 16.0f) return;   /* only the edge spawns are probed */
    Body save = *b;
    float y = b->y;
    int n = 32; bool jump = false;
    while (y > 8.0f) {
        bool hit = true;
        for (int pass = 0; pass < 2 && hit; pass++) {   /* walk first, then the jump */
            Body p = save; p.y = y; p.vx = right ? 120.0f : -120.0f; p.vy = pass ? -250.0f : 0.0f; p.coll = 0;
            for (int k = 0; k < n; k++) { physics_step(W, L, &p, 1.0f / 60.0f); if (p.coll & (COLL_LEFT | COLL_RIGHT)) break; }
            hit = (p.coll & (COLL_LEFT | COLL_RIGHT)) != 0;
            if (!hit && pass) jump = true;
        }
        if (!hit) break;
        n += 8; y -= 8.0f;
    }
    *b = save; b->y = y;
    if (jump) b->vy -= 250.0f;
}

static int count_class(const Enemies *E, int cls) { int n = 0; for (int i = 0; i < MAX_ENEMIES; i++) if (E->e[i].cls == cls) n++; return n; }

/* FUN_00414eb0: called when a convoy horse is spawned or removed */
static void convoy_check(Enemies *E)
{
    if (count_class(E, EC_BUGGY) == 0) { E->cam_shake = false; E->cam_locked = false; E->spawner_enabled = true; E->release_request = true; }
    else { E->cam_shake = true; E->spawner_enabled = false; }
}

/* FUN_00415550: the robot-horse convoy (type 11) */
static Enemy *spawn_convoy(Enemies *E, const Trigger *t, float x, float y, const Level *L, const PhysicsWorld *W)
{
    (void)L; (void)W;
    Character tmp; character_init(&tmp, 0xFBFAF817, false);
    float hx = tmp.box_hx, hy = tmp.box_hy;
    bool right = E->px <= x;         /* horses run toward the player */
    float px0 = right ? x + hx + t->interval_ms * 0.15f : x - hx - t->interval_ms * 0.15f;
    float py0 = right ? y + hy : y - hy;
    float step = t->rand_n * 3.2f + 16.0f;
    int extra = t->remaining_init < 0x39 ? t->remaining_init : 0x38;
    Enemy *head = NULL;
    for (int i = 0; i <= extra; i++) {
        Enemy *e = alloc_slot(E, EC_BUGGY);
        if (!e) break;
        e->type = 11; e->layer = t->layer;
        character_init(&e->ch, 0xFBFAF817, false);
        e->ch.body.x = px0; e->ch.body.y = py0; e->ch.body.flags = 0x1f;
        if (E->px <= px0) character_move_left(&e->ch, 0); else character_move_right(&e->ch, 4);
        if (!head) head = e;
        px0 += right ? step : -step;   /* the column extends away from the player (FUN_00415550) */
    }
    convoy_check(E);
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "convoy spawn: trigger x=%.0f player %.0f first horse %.0f step %.0f speed %.0f\n", x, E->px, head ? head->ch.body.x : -1, step, tmp.speed);
    return head;
}

Enemy *enemy_spawn(Enemies *E, const Trigger *t, float x, float y, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh)
{
    int type = t->type, layer = t->layer;
    if (type == 11) return spawn_convoy(E, t, x, y, L, W);
    int cls;
    switch (type) {
    case 2: case 5: cls = type == 2 ? EC_GRUNT : EC_GRUNT_B; break;
    case 6: case 7: cls = type == 6 ? EC_SNIPER : EC_SNIPER_B; break;
    case 8: case 9: cls = type == 8 ? EC_KNEELER : EC_KNEELER_B; break;
    case 10: cls = EC_HORSEBOSS; break;
    case 11: cls = EC_BUGGY; break;
    case 28: cls = EC_CUTSCENE; break;
    case 29: cls = EC_END; break;
    case 30: case 31: cls = EC_SHIELD; break;
    case 32: cls = EC_STALKER; break;
    default:
        if (type >= 12 && type <= 23) cls = EC_PROP + (type - 12);
        else if (type >= 24 && type <= 27) cls = EC_STAMPEDE + (type - 24);
        else cls = EC_WALKER;
    }
    Enemy *e = alloc_slot(E, cls);
    if (!e) return NULL;
    e->type = type; e->layer = layer;
    bool fall_in = y < -999.0f;
    if (fall_in) y = -1000.0f - y;
    character_init(&e->ch, crhc_for_type(type), fall_in);
    Body *b = &e->ch.body;
    b->x = x + b->hx; b->y = y + b->hy - 4.0f;     /* FUN_00414210: offset by the half extents, 4 px up */
    if (fall_in) b->vy -= 166.6667f;
    e->ch.state = CS_AIR;
    (void)sh;
    e->variant = (type == 5 || type == 7 || type == 9 || type == 29 || type == 32);
    if (cls == EC_GRUNT || cls == EC_GRUNT_B || cls == EC_SNIPER || cls == EC_SNIPER_B || cls == EC_KNEELER || cls == EC_KNEELER_B || cls == EC_END || cls == EC_STALKER) e->gun_alive = true;
    if (cls == EC_STALKER) e->ft = (float)rnd(81);   /* its own spacing: a spot shifted up to +-8 px, a level stand-off 80..160 px */
    if (cls == EC_WALKER || cls == EC_GRUNT || cls == EC_GRUNT_B || cls == EC_STALKER) {
        e->ch.facing = E->px <= b->x ? 0 : 1;
        face_and_probe(e, e->ch.facing == 1, L, W, cam_x, sw);
        snap_to_ground(e, L, W);
        if (e->ch.flags & CF_SPAWN_FALL) e->spawn_t = 0.6666667f;
    } else if (cls >= EC_PROP && cls <= EC_STAMPEDE + 3) {
        b->x = x; b->y = y; b->vx = b->vy = 0; b->flags = 0x1f;
        e->ch.state = CS_IDLE;
        if (cls >= EC_STAMPEDE) { if (E->px <= b->x) character_move_left(&e->ch, 0); else character_move_right(&e->ch, 4); }
    } else if (cls == EC_HORSEBOSS) {
        b->x = x; b->y = y; b->flags = 0x0f | PHYS_NO_GRAVITY;
        e->hp = SDL_getenv("SABER_BOSSHP") ? atoi(SDL_getenv("SABER_BOSSHP")) : 0x42; e->link = -1; e->ch.state = CS_FALL; e->ch.facing = 0; e->ch.aim = AIM_L;   /* debug: SABER_BOSSHP=n */
        E->boss_phase = 0;
    } else if (cls == EC_SHIELD) {
        e->ch.facing = E->px <= b->x ? 0 : 1;
        snap_to_ground(e, L, W);
        e->hp = E->difficulty == 0 ? 4 : E->difficulty == 1 ? 6 : 8;   /* shots the shield takes */
        e->t0 = 0.9f + rnd(30) * 0.02f;                                 /* first shot once on screen */
        e->dormant = type == 31;
        int cx = (int)floorf(b->x / L->cellw), cy = (int)floorf((sh_feet(e) + 1.0f) / L->cellh);
        e->on_deck = L->collision && cx >= 0 && cx < L->cols && cy >= 0 && cy < L->rows && L->collision[cy * L->cols + cx] == 4;   /* one-way floor, no ramp */
    } else if (cls == EC_CUTSCENE) {
        snap_to_ground(e, L, W);
        e->ch.facing = 0; e->ch.state = CS_IDLE; e->ch.aim = AIM_L;
    } else {
        e->ch.facing = E->px <= b->x ? 0 : 1;
        snap_to_ground(e, L, W);
    }
    return e;
}

static void spawner_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh, float dt)   /* FUN_00421370 */
{
    Body *pb = &pl->ch.body;
    float pcx = pb->x + pb->ox, pcy = pb->y + pb->oy;
    if (!E->spawner_enabled) return;
    for (int i = 0; i < E->ntr; i++) {
        Trigger *t = &E->tr[i];
        if (fabsf(pcx - t->cx) > t->hx + pb->hx || fabsf(pcy - t->cy) > t->hy + pb->hy) continue;
        t->timer -= dt;
        while (t->timer <= 0.0f && t->remaining != 0) {
            t->timer = t->interval_ms * 0.001f + (t->rand_n ? rnd(t->rand_n + 1) * 0.02f : 0);
            t->remaining--;
            float wx = t->wp[t->spawned % t->nwp][0], wy = t->wp[t->spawned % t->nwp][1];
            t->spawned++;
            const Character tmp = {0}; (void)tmp;
            /* +-100000 = just outside the screen, offset by the sprite origin */
            float ox = 32, oy = 32;
            if (wx > 99999.0f) wx = (cam_x + sw) + ox; else if (wx < -99999.0f) wx = cam_x - ox;
            if (wy > 99999.0f) wy = oy + sh; else if (wy < -99999.0f) wy = -oy;
            Enemy *e = enemy_spawn(E, t, wx, wy, L, W, cam_x, sw, sh);
            if (t->type == 11) t->cy = -10000.0f;    /* buggy triggers fire once */
            if (!e) break;
        }
    }
}

/* ---- class updates ---- */
static bool hurt_overlap(const Character *a, float ax, float ay, const Enemies *E)
{
    const HurtBox *h = &a->hurt[a->anim < CHAR_MAX_ANIMS ? a->anim : 0];
    float cx = ax + h->ox, cy = ay + h->oy;
    return fabsf(E->phcx - cx) <= E->phx + h->hw && fabsf(E->phcy - cy) <= E->phy + h->hh;
}

bool player_damage(Player *pl, int hit_dir, int dmg)   /* FUN_00422a10 */
{
    Character *c = &pl->ch;
    if (dmg < 1) return false;
    if (dmg <= pl->hp) { pl->hp -= dmg; c->flags |= CF_HIT; c->hit_t = 170.0f; sfx_play(3, 0); return true; }
    if ((hit_dir & 0xfb) != 2) {
        if (hit_dir < 8 && ((1u << hit_dir) & 0x83u)) { c->facing = 1; c->body.vx -= 300.0f; }
        else { c->facing = 0; c->body.vx += 300.0f; }
    }
    c->state = CS_DEAD;
    sfx_play(4, 0);
    return true;
}

/* common tail of the humanoid enemy updates: player contact, player bullets, death floor, resolve, knockback */
static void humanoid_tail(Enemies *E, Enemy *e, Player *pl, Bullets *pb, float cam_x, int sw, float dt, bool offscreen)
{
    Character *c = &e->ch; Body *b = &c->body;
    int knock = 0; bool die = offscreen;
    if ((c->flags & CF_SPAWN_FALL) && e->spawn_t > 0) {
        e->spawn_t -= dt; if (e->spawn_t < 0) e->spawn_t = 0;
        goto resolve;
    }
    if (hurt_overlap(c, b->x, b->y, E)) {
        Character *p = &pl->ch;
        if (p->state == CS_SLIDE) {
            if (c->state != CS_DEAD) { if (p->facing == 0) { e->ch.facing = 1; knock = -1; } else { e->ch.facing = 0; knock = 1; } }
            die = true; c->state = CS_DEAD;
        } else if (!(p->flags & CF_HIT) && p->state != CS_DEAD) {
            player_damage(pl, e->ch.facing == 0 ? 0 : 4, 1);
        }
    }
    {
        const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
        float cx = b->x + h->ox, cy = b->y + h->oy;
        for (int i = 0; i < pb->n; i++) {
            Bullet *bl = &pb->b[i];
            float r = bl->kind == BK_GRENADE ? 8.0f : 5.0f;
            if (fabsf(cx - bl->x) > r + h->hw || fabsf(cy - bl->y) > r + h->hh) continue;
            float sx = bl->x - cam_x; if (sx <= 8.0f || sx >= sw) continue;
            int d = bl->dir;
            pb->b[i] = pb->b[--pb->n];
            if (c->state != CS_DEAD && (d & 0xfb) != 2) {
                if (d < 8 && ((1u << d) & 0x83u)) { e->ch.facing = 1; knock = -1; } else { e->ch.facing = 0; knock = 1; }
            }
            die = true; c->state = CS_DEAD;
            break;
        }
    }
resolve:
    if (b->y - b->hy > E->death_floor) { die = true; c->state = CS_DEAD; }
    character_resolve(c, dt);
    {   /* FUN_0041ef20 (unconditional in the original): the shoot pose is dropped only on the frame the cooldown
         * runs out (an armed request keeps CF_SHOOT while cd == 0, e.g. the kneeler's 0.235 s wind-up). Gating this
         * on gun_alive left a grunt that had finished its burst (gun_alive = false) in the shooting pose for good:
         * it walked off with the standing-shoot cell, legs frozen. */
        float was = e->gun_cd; e->gun_cd -= dt; if (e->gun_cd < 0) e->gun_cd = 0;
        if (was > 0 && e->gun_cd <= 0) c->flags &= ~CF_SHOOT;
    }
    if (knock) b->vx += knock * 102.0f;
    if (die && c->state == CS_DEAD && !e->dying) { sfx_play(5, 0); sfx_play(6, 3); }
    if (die) kill(E, e, 0x19f);
}

static void update_walker(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, float cam_x, int sw, float dt)   /* FUN_004144d0 */
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)L;
    character_sync_ground(c);
    bool offscreen = false;
    if (e->ch.facing == 1) {
        if (!(c->coll & COLL_RIGHT)) { character_move_right(c, 4); offscreen = b->x - b->hx > cam_x + sw; }
        else character_move_left(c, 0);
    } else if (e->ch.facing == 0) {
        if (c->coll & COLL_LEFT) {
            if (W->world_min_x <= b->x - b->hx - 1.0f) character_move_right(c, 4);
            else { b->flags = PHYS_IGNORE_LEFT; character_move_left(c, 0); }
        } else { character_move_left(c, 0); offscreen = b->x + b->hx < W->world_min_x; }
    } else character_move_left(c, 0);
    humanoid_tail(E, e, pl, pb, cam_x, sw, dt, offscreen);
}

/* FUN_00416d20: grunt (types 2/5). The shooting phase lives in ch.speed (tiny while shooting -> stands still). */
static void update_grunt(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)L;
    character_sync_ground(c);
    bool offscreen = false;
    float quarter = 0.125f * sw * 2;   /* 0x7c43f8 * screen_w, screen_w is the logical width */
    quarter = 0.125f * sw;
    if (c->speed < 1.0f) {
        c->speed += 1e-5f;
        if (c->speed > 0.00016f) {
            if (c->speed < 0.00017f) {
                float mx = e->ch.facing == 0 ? (e->variant ? -29.0f : -26.0f) : (e->variant ? 29.0f : 26.0f);
                if (e->gun_alive && e->gun_cd <= 0.0f) {
                    float x = b->x + mx, y = b->y + c->muzzle_y;
                    AnimDef fl = { 0, 0, 3, 0, 0.05f, 0 };
                    Effect *ef = effects_spawn(fx, 0xD85FB68A, e->layer, &fl, x, y, 8, 8, c->aim == AIM_L ? 3.1415927f : 0);
                    if (ef) { ef->follow_x = &b->x; ef->follow_y = &b->y; ef->fx0 = b->x; ef->fy0 = b->y; }
                    bullets_spawn(eb, BK_ENEMY, e->layer, x, y, c->aim & 7, 166.0f);
                    e->gun_cd = 0.2f;
                    sfx_play(7, 0);
                }
            } else if (c->speed > 0.00024f) {
                c->speed = 120.0f; e->gun_alive = false;
                if (e->ch.facing == 0) character_move_right(c, 4); else character_move_left(c, 0);
            }
        }
    } else if (e->ch.facing == 0) {
        if (c->coll & COLL_LEFT) {
            if (W->world_min_x <= b->x - b->hx - 1.0f) { e->gun_alive = false; character_move_right(c, 4); }
            else { b->flags = PHYS_IGNORE_LEFT; character_move_left(c, 0); }
        } else {
            character_move_left(c, 0);
            if (b->x + b->hx < W->world_min_x) offscreen = true;
            else if (c->state != CS_AIR && E->px + quarter < b->x && b->x < cam_x + sw - quarter && rnd(100) >= 0x60 && e->gun_alive
                     && character_request_shoot(c)) c->speed = 0;
        }
    } else if (e->ch.facing == 1) {
        if (c->coll & COLL_RIGHT) { e->gun_alive = false; character_move_left(c, 0); }
        else {
            character_move_right(c, 4);
            if (b->x - b->hx > cam_x + sw) offscreen = true;
            else if (c->state != CS_AIR && E->px - quarter > b->x && b->x > cam_x + quarter && rnd(100) >= 0x60 && e->gun_alive
                     && character_request_shoot(c)) c->speed = 0;
        }
    } else character_move_left(c, 0);
    humanoid_tail(E, e, pl, pb, cam_x, sw, dt, offscreen);
}

/* shared enemy shot (FUN_0041ebd0 with kind 1): flash D85FB68A frames 0..3 @50ms, bullet 166 px/s, 0.2 s cooldown */
static bool enemy_fire(Enemy *e, Bullets *eb, Effects *fx, float mx, float my)
{
    Character *c = &e->ch; Body *b = &c->body;
    if (!e->gun_alive || e->gun_cd > 0.0f) return false;
    float x = b->x + mx, y = b->y + my;
    AnimDef fl = { 0, 0, 3, 0, 0.05f, 0 };
    static const float ANG[8] = { 3.1415927f, 2.3561945f, 1.5707964f, 0.7853982f, 0, 5.4977871f, 4.712389f, 3.9269908f };
    Effect *ef = effects_spawn(fx, 0xD85FB68A, e->layer, &fl, x, y, 8, 8, ANG[c->aim & 7]);
    if (ef) { ef->follow_x = &b->x; ef->follow_y = &b->y; ef->fx0 = b->x; ef->fy0 = b->y; }
    bullets_spawn(eb, BK_ENEMY, e->layer, x, y, c->aim & 7, 166.0f);
    e->gun_cd = 0.2f;
    sfx_play(7, 0);
    return true;
}

/* FUN_00419910: sniper (types 6/7). Stands aiming at the player; ch.speed is the aim-settle timer. */
static void update_sniper(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)L; (void)W;
    float px = E->phcx, py = E->phcy, pvy = pl->ch.body.vy;
    character_sync_ground(c);
    if (c->state == CS_IDLE) character_aim_stand(c);
    if (c->state == CS_AIM) {
        float t = c->speed;
        bool try_shoot = false;
        if (t <= 0.125f) {
            t += 0.01f;
        } else {
            float dx = fabsf(px - b->x);
            float lead = (pvy > -150.0f && pvy < 70.0f && (pl->ch.flags & CF_IN_JUMP)) ? 20.0f : 0.0f;
            float dyq = floorf(fabsf(lead + (py - b->y)) * 0.0625f) * 16.0f;
            bool below = b->y < py;      /* player below the enemy */
            int want;
            float newt;
            if (dyq < dx || dx > 16.0f) {
                float th = below ? 24.0f : E->aim_decks ? 40.0f : 48.0f;
                if (th < dyq || (dx > 0.1f && dyq / dx > 2.0f)) want = below ? (b->x <= px ? AIM_DR : AIM_DL) : (b->x <= px ? AIM_UR : AIM_UL);
                else want = b->x <= px ? AIM_R : AIM_L;
                newt = 0.01f;
            } else if (dyq <= 0.1f || dx / dyq <= 0.1f) {
                want = below ? AIM_D : AIM_U; newt = 0.0725f;
            } else {
                want = below ? (b->x <= px ? AIM_DR : AIM_DL) : (b->x <= px ? AIM_UR : AIM_UL); newt = 0.0725f;
            }
            if (c->aim != want) { character_aim(c, want); character_aim_stand(c); t = newt; }
            else if (t > 0.2f && c->state != CS_AIR) try_shoot = true;
            else t += 0.01f;
        }
        if (try_shoot) {
            if (cam_x < b->x && b->x < cam_x + sw && rnd(200) > 0xc5 && character_request_shoot(c)) {
                enemy_fire(e, eb, fx, c->muzzle_x, c->muzzle_y);
                t = 0;
            }
        }
        c->speed = t;
    }
    humanoid_tail(E, e, pl, pb, cam_x, sw, dt, false);
}

/* ours, stage 4's finale: the blue Outrider (type 32, the sniper's body) runs in from off screen like a grunt, then
 * takes a firing line on the hero and shoots like a sniper. With the hero up on a tower deck the line is 45 degrees:
 * it stands where the up-diagonal from its muzzle crosses his middle (the muzzle's rise = its run); with the hero on
 * the ground it stops 80..160 px short of him and fires level. It re-picks the spot only while he stands (not in a
 * jump) and walks over when he leaves the line. st 0 = walking to t0, 1 = aiming; t1 = time to the next shot;
 * dir = the aim for t0. */
#define STALK_MZ_X 17.8f   /* the sniper body's 45-degree-up muzzle from the body centre (UR; UL mirrored) */
#define STALK_MZ_Y 20.0f
static float stalk_spot(const Enemies *E, const Enemy *e, float cam_x, int sw, int *aim)
{
    const Body *b = &e->ch.body;
    float lo = cam_x + 24.0f, hi = cam_x + sw - 24.0f;
    float rise = (b->y - STALK_MZ_Y) - E->phcy;
    if (rise > 24.0f) {                                      /* he is up high: the diagonal spot nearer to us */
        float d = rise + STALK_MZ_X + (e->ft / 80.0f * 16.0f - 8.0f);
        float l = E->phcx - d, r = E->phcx + d;              /* from the left it fires up-right */
        bool okl = l >= lo, okr = r <= hi;
        if (okl || okr) {
            bool left = okl && (!okr || fabsf(b->x - l) <= fabsf(b->x - r));
            *aim = left ? AIM_UR : AIM_UL; return left ? l : r;
        }
    }
    float gap = 80.0f + e->ft;                               /* level with him */
    bool left = b->x < E->phcx;
    float x = E->phcx + (left ? -gap : gap);
    if (x < lo || x > hi) { left = !left; x = E->phcx + (left ? -gap : gap); }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    float cur = b->x - E->phcx;                              /* already in the band on that side: stay */
    if ((left ? -cur : cur) >= 70.0f && (left ? -cur : cur) <= 170.0f && b->x >= lo && b->x <= hi) x = b->x;
    *aim = x < E->phcx ? AIM_R : AIM_L;
    return x;
}

static void update_stalker(Enemies *E, Enemy *e, Player *pl, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    character_sync_ground(c);
    bool grounded = (c->coll & COLL_DOWN) != 0;
    if (grounded && (pl->ch.coll & COLL_DOWN) && pl->ch.state != CS_DEAD && !(c->flags & CF_SHOOT)) {
        int aim; float x = stalk_spot(E, e, cam_x, sw, &aim);
        if (e->st == 0 || fabsf(x - b->x) > 10.0f || aim != e->dir) { e->t0 = x; e->dir = (uint8_t)aim; if (fabsf(x - b->x) > 3.0f) e->st = 0; }
    }
    if (e->st == 0 && grounded) {
        if (fabsf(b->x - e->t0) <= 3.0f && b->x > cam_x + 8.0f && b->x < cam_x + sw - 8.0f) {
            e->st = 1; e->t1 = 0.35f + rnd(10) * 0.02f;      /* at the spot: raise the rifle, settle, then fire */
        } else {
            c->speed = 110.0f;
            if (e->t0 < b->x) character_move_left(c, 0); else character_move_right(c, 4);
        }
    }
    if (e->st == 1) {
        character_aim(c, e->dir); character_aim_stand(c);
        if ((e->t1 -= dt) <= 0 && b->x > cam_x && b->x < cam_x + sw && pl->ch.state != CS_DEAD && character_request_shoot(c)) {
            enemy_fire(e, eb, fx, c->muzzle_x, c->muzzle_y);
            e->t1 = 0.8f + rnd(25) * 0.03f;
        }
    }
    humanoid_tail(E, e, pl, pb, cam_x, sw, dt, false);
}

/* FUN_00418670: kneeler (types 8/9/29). Crouches and throws grenades; ch.speed is the throw timer. */
static void update_kneeler(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)L; (void)W;
    float px = E->phcx, py = E->phcy;
    character_sync_ground(c);
    if (c->state == CS_IDLE) character_down(c);
    if (c->state == CS_CROUCH) {
        character_aim(c, b->x <= px ? AIM_R : AIM_L);
        e->ch.facing = (c->aim < 8 && ((1u << c->aim) & 0x83u)) ? 0 : 1;
        if (c->speed > 1.0f) {
            if (c->state != CS_AIR && cam_x < b->x && b->x < cam_x + sw && rnd(100) >= 0x5d && character_request_shoot(c))
                c->speed = 0;
        } else {
            c->speed += 0.01f;
            if ((c->flags & CF_SHOOT) && c->speed > 0.235f) {
                float lead = b->x < px ? 60.0f : 0.0f;
                float pxm = pl->ch.facing == 0 ? px - 60.0f : px + 6.0f;   /* 0x7c3b5c=60, 0x7c440c=6 */
                float dx = fabsf(lead - b->x + pxm);
                if (dx < 80.0f) dx = 80.0f;
                if (dx > 200.0f) dx = 200.0f;
                float dy = fabsf(py - b->y) * 0.25f;
                if (py < b->y) dy *= -10.0f;
                float th = pl->ch.facing == 0 ? 320.0f : 230.0f;
                float div = th < dx ? 1.2333333f : 1.125f;
                float spd = rnd(6) + (dx - dy) / div - 3.0f;
                if (spd < 60.0f) spd = 60.0f;
                if (spd > 160.0f) spd = 160.0f;
                if (e->gun_alive && e->gun_cd <= 0.0f) {
                    float x = b->x + c->muzzle_x, y = b->y + c->muzzle_y;
                    bullets_spawn(eb, BK_GRENADE, e->layer, x, y, c->aim & 7, spd);
                    e->gun_cd = 0.2f;
                    sfx_play(13, 0);
                }
                c->speed += 0.235f;
            }
        }
    }
    humanoid_tail(E, e, pl, pb, cam_x, sw, dt, false);
}

/* FUN_00411aa0: decorative props (types 12..23): static, loop anim entry 1, despawn a screen behind the camera */
static void update_prop(Enemies *E, Enemy *e, float cam_x, int sw)
{
    Character *c = &e->ch;
    c->body.flags = 0x1f;
    character_set_anim(c, 1);
    if (c->body.x < cam_x - sw) kill(E, e, 0x19f);
}

/* FUN_004121a0: stampede horses (types 24..27): run across the screen at a fixed pixel speed per frame */
static void update_stampede(Enemies *E, Enemy *e, float cam_x, int sw)
{
    static const float SPEED[4] = { 7.875f, 4.4625f, 9.1875f, 5.25f };   /* 0x7c4194 (22), 0x7c4198 (23), 0x7c419c (24), 0x7c4190 (25) px/frame */
    Character *c = &e->ch;
    c->body.flags = 0x1f;
    float sp = SPEED[e->cls - EC_STAMPEDE];
    bool right = c->aim == AIM_R;
    character_set_anim(c, right ? 2 : 1);
    if (right) { c->body.x += sp; if (c->body.x - 2 * c->origin_x > cam_x + sw) kill(E, e, 0x19f); }
    else { c->body.x -= sp; if (c->body.x + 2 * c->origin_x < cam_x) kill(E, e, 0x19f); }
}

/* FUN_00415c10: convoy horse. Forced walk state, no gravity/collision, tramples the player and humanoid enemies. */
static void update_horse(Enemies *E, Enemy *e, Player *pl, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    b->flags = 0x1f; b->coll = COLL_DOWN;
    c->state = CS_WALK;
    character_sync_ground(c);
    c->state = CS_WALK;
    bool offscreen = false;
    if (e->ch.facing == 1) { character_move_right(c, 4); offscreen = b->x - b->hx > cam_x + sw; }
    else { character_move_left(c, 0); offscreen = b->x + b->hx < cam_x; }
    c->state = CS_WALK;
    /* player */
    if (hurt_overlap(c, b->x, b->y, E)) {
        Character *p = &pl->ch;
        if (!(p->flags & CF_HIT) && p->state != CS_DEAD) player_damage(pl, e->ch.facing == 0 ? 0 : 4, 1);
    }
    /* humanoid enemies */
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float cx = b->x + h->ox, cy = b->y + h->oy;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *o = &E->e[i];
        if (!o->cls || o->dying || o->cls >= 8 || o == e) { if (!(o->cls == EC_END && !o->dying)) continue; }
        const HurtBox *oh = &o->ch.hurt[o->ch.anim < CHAR_MAX_ANIMS ? o->ch.anim : 0];
        float ox = o->ch.body.x + oh->ox, oy = o->ch.body.y + oh->oy;
        if (fabsf(ox - cx) > oh->hw + h->hw || fabsf(oy - cy) > oh->hh + h->hh) continue;
        o->ch.facing = e->ch.facing == 0 ? 1 : 0;
        o->ch.state = CS_DEAD; sfx_play(5, 0); sfx_play(6, 3);
        character_resolve(&o->ch, dt);
        o->ch.body.vx += e->ch.facing == 0 ? -102.0f : 102.0f;
        kill(E, o, 0x19f);
    }
    character_resolve(c, dt);
    if (offscreen) kill(E, e, 0x19f); else sfx_play(20, 0);
}

/* FUN_00417b60: the cutscene Outrider on the Ramrod's roof (type 28). Alarm pose when seen, then runs off; despawns
 * once it has left the screen. ft = run flag (50 while running, +0x12848). */
static void update_cutscene_outrider(Enemies *E, Enemy *e, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    float camc = cam_x + sw * 0.5f;
    if ((int)fabsf(camc - b->x) < sw / 2) {
        if (c->state == CS_IDLE) { c->state = CS_SLIDE; c->slide_t = 2.0f; sfx_play(22, 0); }
        else if (c->state == CS_SLIDE && c->slide_t < 1.0f) { c->state = CS_WALK; e->ch.facing = 1; c->aim = AIM_R; c->slide_t = 0; c->alert_time = 50.0f; e->ft = 50.0f; }
    } else {
        if (e->ft >= 40.0f) kill(E, e, 0x19f);      /* ran off the screen */
        else { e->ft = 0; b->vx = b->vy = 0; }
    }
    character_sync_ground(c);
    if (e->ft > 40.0f) { character_move_right(c, 4); e->ft = 50.0f; }
    character_resolve(c, dt);
}

/* ---- Ramrod horse mini-boss (FUN_00412510 / FUN_00412750). Structure ported, timings approximated. ---- */
static int next_sprite_layer(const Level *L, int layer)
{
    for (int i = layer + 1; i < L->nlayers; i++) if (!L->layers[i].is_tilemap) return i;
    return -1;
}

static void boss_fire(Enemies *E, Enemy *e, Bullets *eb, int sound)
{
    Character *c = &e->ch;
    if (e->gun_cd > 0.0f) return;
    bullets_spawn(eb, BK_LASER, e->layer, c->body.x + c->muzzle_x, c->body.y + c->muzzle_y, c->aim & 7, 333.3333f);
    e->gun_cd = 0.2f; c->flags |= CF_SHOOT;
    sfx_play(sound, 0);
    (void)E;
}

static void update_boss(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)W;
    E->frame++;
    if (e->dying) {
        /* FUN_00412750 state 9: the hover is not applied any more and the body keeps its collision-less flags (0xf)
         * with gravity on, so the wreck drops through the floor and out of the screen while it burns, jittering
         * sideways (the port used to keep hovering in place here) */
        c->state = CS_DEAD;
        b->flags = 0x0f; b->vx = 0;
        if (SDL_getenv("SABER_TRACE") && (E->boss_phase % 10) == 0) fprintf(stderr, "boss dying phase=%d x=%.0f y=%.0f vy=%.0f\n", E->boss_phase, b->x, b->y, b->vy);
        b->x += (float)(rnd(180)) / 60.0f - 153.0f / 60.0f;
        if (rnd(10) >= 9) {
            AnimDef a = { 0, 0, 11, 11, 0.025f, 0 };
            effects_spawn(fx, 0x9C861FF3, e->layer, &a, b->x + rnd(18), b->y + rnd(80), 32, 32, 0);
            if (E->boss_phase < 0x40 && (E->boss_phase & 1)) sfx_play(0x11, 0);
        }
        if (E->boss_phase == 8) sfx_play(0x15, 0);
        E->boss_phase++;
        character_set_anim(c, c->facing ? 0x33 : 0x32);
        return;
    }
    /* hover: vertical sine, gravity cancelled */
    float amp = c->state == CS_FALL ? 10.0f : c->state == CS_SLIDE ? 13.0f : 16.0f;
    b->flags = 0x0f | PHYS_NO_GRAVITY;
    b->vy = sinf((float)(E->frame % 66) * 0.0952f) * amp;
    if (E->boss_phase == 0) { E->boss_phase = 1; E->cam_locked = true; music_play(8, true); sfx_play(0x13, 0); }
    if (SDL_getenv("SABER_TRACE") && (E->frame % 10) == 0)
        fprintf(stderr, "boss st=%d layer=%d x=%.0f y=%.0f vx=%.0f cam=%.0f phase=%d dir=%d aim=%d hp=%d anim=%d\n", c->state, e->layer, b->x, b->y, b->vx, cam_x, E->boss_phase, e->dir, c->aim, e->hp, c->anim);

    switch (c->state) {
    case CS_FALL:      /* fly-in from the right in the far layer, accelerating left (0x7c4200) */
        b->vx += dt * -18666.0f;
        c->facing = 0; c->aim = AIM_L;
        if (b->x + 350.0f < cam_x) {
            int nl = next_sprite_layer(L, e->layer);
            if (nl >= 0) e->layer = nl;
            c->state = CS_SLIDE; e->dir = 1; e->ft = 400.0f; c->speed = 140.0f; E->boss_phase = 0; sfx_play(0x13, 0);
            b->x = cam_x - 820.0f; b->vx = 0;   /* re-enters at the left edge ~1.9 s later (measured on the original) */
        }
        break;
    case CS_SLIDE:     /* mid layer, accelerating right (0x7c4208) */
        b->vx += dt * 23332.75f;
        c->facing = 1; c->aim = AIM_R;
        if (b->x - 230.0f > cam_x + sw) {
            int nl = next_sprite_layer(L, e->layer);
            if (nl >= 0) { e->layer = nl; b->x = cam_x - 230.0f; b->vx = 0; }
            if (nl < 0 || next_sprite_layer(L, nl) < 0) {
                /* reached the play layer: the boss sweeps (state 1, facing left, aiming down-left) and a clone of
                 * it (FUN_0041fbf0) rides along in state 8 as a second gun; both share the phase counter */
                e->layer = nl >= 0 ? nl : e->layer;
                c->state = CS_WALK; c->aim = AIM_DL; e->dir = 0; c->facing = 0; b->x = cam_x + sw + 280.0f; b->vx = 0;   /* ~1.5 s until it shows */
                Enemy *r = alloc_slot(E, EC_HORSEBOSS);
                if (r) { character_init(&r->ch, 0x2A02BD4F, false); r->type = 10; r->layer = e->layer; r->link = (int)(e - E->e); r->hp = 1; r->variant = 99; r->ch.state = CS_AIM; r->ch.aim = AIM_DR; r->ch.body = *b; e->link = (int)(r - E->e); }
                E->boss_phase = 1;
            }
        }
        break;
    case CS_WALK: {    /* FUN_00412750 state 1: fly in to the centre (+128 / -160), hold and shoot for 0x50 frames,
                        * then leave on the far side and come back from there. The resolver turns c->speed into vx. */
        float camc = cam_x + sw * 0.5f;
        bool can_shoot = false;
        if (e->dir == 0) {
            if (E->boss_phase > 0x50) {
                c->speed = 140.0f; E->boss_phase++;
                if (b->x + 180.0f < cam_x) { e->dir = 1; c->aim = AIM_DR; E->boss_phase = 1; }
            } else if (E->boss_phase < 2 && b->x >= camc) {
                if (b->x > camc + 128.0f) { c->speed = 140.0f; can_shoot = true; } else { E->boss_phase++; c->speed = 0; }
            } else { E->boss_phase++; c->speed = 0; can_shoot = true; }
        } else {
            if (E->boss_phase > 0x50) {
                c->speed = 140.0f; E->boss_phase++;
                if (b->x - 280.0f > cam_x + sw) { e->dir = 0; c->aim = AIM_DL; E->boss_phase = 1; }
            } else if (E->boss_phase < 2 && b->x <= camc) {
                if (b->x < camc - 160.0f) { c->speed = 140.0f; can_shoot = true; } else { E->boss_phase++; c->speed = 0; }
            } else { E->boss_phase++; c->speed = 0; can_shoot = true; }
        }
        c->facing = e->dir; c->aim = e->dir ? AIM_DR : AIM_DL;
        if (can_shoot && b->x > cam_x - 64.0f && b->x < cam_x + sw) boss_fire(E, e, eb, 0x10);
        break; }
    default:
        c->state = CS_FALL;
        break;
    }
    /* player bullets: only the sweeping boss takes hits (state 1); the passes and the rider are invulnerable */
    if (c->state == CS_WALK) {
        const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
        float cx = b->x + h->ox, cy = b->y + h->oy;
        float hw = h->hw > 0 ? h->hw : 48, hh = h->hh > 0 ? h->hh : 32;
        for (int i = 0; i < pb->n; i++) {
            Bullet *bl = &pb->b[i];
            float r = bl->kind == BK_GRENADE ? 8.0f : 5.0f;
            if (fabsf(cx - bl->x) > r + hw || fabsf(cy - bl->y) > r + hh) continue;
            float sx = bl->x - cam_x; if (sx <= 8.0f || sx >= sw) continue;
            pb->b[i] = pb->b[--pb->n];
            if (c->flags & (CF_HIT | CF_HIT_ALT)) break;
            if (--e->hp <= 0) {
                kill(E, e, 0xed8); c->state = CS_DEAD; E->boss_phase = 1; c->flags &= ~CF_SHOOT;
                if (e->link >= 0 && E->e[e->link].cls) { E->e[e->link].cls = 0; E->count--; e->link = -1; }
            } else {
                c->flags |= CF_HIT; c->hit_t = 4.0f;
                if (e->link >= 0 && E->e[e->link].cls) { E->e[e->link].ch.flags |= CF_HIT; E->e[e->link].ch.hit_t = 4.0f; }
            }
            break;
        }
    }
    e->gun_cd -= dt; if (e->gun_cd < 0) e->gun_cd = 0;
    if (e->gun_cd < 0.1f) c->flags &= ~CF_SHOOT;
    /* animation from the enemy resolver (CRHC anims 28-35 = laser charge, etc.) */
    uint8_t st = c->state;
    if (st == CS_FALL || st == CS_SLIDE) {
        /* the passes keep their accelerated velocity (the walk resolver would pin it to +-speed); the original
         * crosses the screen at ~390 px/s */
        float vx = b->vx; if (vx > 390.0f) vx = 390.0f; if (vx < -390.0f) vx = -390.0f;
        c->state = CS_WALK; character_resolve(c, dt); c->state = st; b->vx = vx;
    } else character_resolve(c, dt);
    (void)pl;
}

/* FUN_00412750 state 8: the rider clone sits on the boss (position copied from the link), takes the boss's facing
 * and covers the other side: boss facing right -> aims left (down-left when the player is 60+ px below), facing
 * left -> aims right / down-right. It holds fire while the shared phase counter is in 0xa0..0xb8. */
static void update_boss_rider(Enemies *E, Enemy *e, Bullets *eb, float cam_x, int sw, float dt)
{
    Character *c = &e->ch;
    if (e->link < 0 || !E->e[e->link].cls) { e->cls = 0; E->count--; return; }
    Enemy *h = &E->e[e->link];
    c->body = h->ch.body; c->body.flags = 0x1f;
    e->layer = h->layer;
    c->state = CS_AIM;
    e->dir = h->dir; c->facing = h->ch.facing;
    bool below = E->phcy - c->body.y >= 60.0f;
    c->aim = e->dir == 1 ? (below ? AIM_DL : AIM_L) : (below ? AIM_DR : AIM_R);
    if (h->ch.state == CS_WALK && !h->dying && !((unsigned)(E->boss_phase - 0xa0) < 0x19) && c->body.x > cam_x - 64.0f && c->body.x < cam_x + sw)
        boss_fire(E, e, eb, 0x12);
    e->gun_cd -= dt; if (e->gun_cd < 0) e->gun_cd = 0;
    if (e->gun_cd < 0.1f) c->flags &= ~CF_SHOOT;
    c->flags = (c->flags & ~(CF_HIT | CF_HIT_ALT)) | (h->ch.flags & (CF_HIT | CF_HIT_ALT)); c->hit_t = h->ch.hit_t;
    character_resolve(c, dt);
}

/* ---- stage 4: the Outrider behind a riot shield (type 30, art from forest/sniper.py) ----
 * Stands its ground facing the hero and fires level rifle shots. The shield soaks up shots from the front until it
 * burns away (sheet frames 1..7); the head above it and the back are open, so a jumping shot, a shot down from a
 * tower deck or one from behind drops him with the shield still up. A hero behind him for a moment makes him turn
 * round. Dies like the Outriders (knocked back, red, cyan, vapour: sheet 8..13 without the shield, 14..19 with it).
 * Up in a tower with the hero on the ground ahead he aims 45 degrees down (sheet 20 with the shield, 21 without, 23..34
 * the deaths from it) and the rifle, its shots and the flash are drawn over the cabin's front wall (sheet 22).
 * st 0 shield up, 1 burning away (t1 = time), 2 no shield; t0 = time to the next shot; ft = time the hero is behind. */
#define SH_AX 24.0f        /* body.x in the art (the torso's middle), art facing right */
#define SH_FEET 52.0f      /* the feet rest on this art row */
static const float SH_BODY[4] = { 14, 9, 35, 53 };      /* art boxes x0, y0, x1, y1 */
static const float SH_SHIELD[4] = { 35, 15, 53, 57 };
#define SH_MUZZLE_DX 28.0f      /* level shot: from the muzzle, art (52, 20) */
#define SH_MUZZLE_DY 32.0f
#define SH_DOWN_DX 18.0f        /* aimed down: the muzzle at art (41, 43) */
#define SH_DOWN_DY 9.0f
#define SH_BREAK_FRAME 0.1f      /* the clip's rate */
#define SH_DEATH_FRAME 0.0667f   /* the Outriders' death anim (0x32/0x33) rate */

static float sh_feet(const Enemy *e) { const Body *b = &e->ch.body; return b->y + b->oy + b->hy; }
static void sh_box(const Enemy *e, const float a[4], float o[4])
{
    float x = e->ch.body.x, f = sh_feet(e);
    if (e->ch.facing == 1) { o[0] = x + a[0] - SH_AX; o[2] = x + a[2] - SH_AX; }
    else { o[0] = x - (a[2] - SH_AX); o[2] = x - (a[0] - SH_AX); }
    o[1] = f + a[1] - SH_FEET; o[3] = f + a[3] - SH_FEET;
}
static bool sh_shield_up(const Enemy *e) { return e->st == 0 || (e->st == 1 && e->t1 < 3 * SH_BREAK_FRAME); }   /* the panel is gone from frame 4 */

static void update_shield(Enemies *E, Enemy *e, Player *pl, Bullets *pb, Bullets *eb, Effects *fx, float cam_x, int sw, float dt)
{
    Character *c = &e->ch; Body *b = &c->body;
    Character *p = &pl->ch;
    b->vx = 0;
    if (e->flash > 0) e->flash -= dt;
    /* type 31 (the finale's tower): stands there, no shots, and the hero's shots fly through him until woken */
    if (e->dormant) {
        if (!E->shield_wake) return;
        e->dormant = false; e->t0 = 0.6f + rnd(20) * 0.02f;
    }
    if (e->st == 1 && (e->t1 += dt) >= 6 * SH_BREAK_FRAME) e->st = 2;
    /* turns to a hero who got behind him */
    bool behind = c->facing == 1 ? E->phcx < b->x - 6.0f : E->phcx > b->x + 6.0f;
    if (behind && p->state != CS_DEAD) { if ((e->ft += dt) > 0.9f) { c->facing ^= 1; e->ft = 0; } } else e->ft = 0;
    /* up on a tower deck with the hero down on the ground ahead: the rifle points down at 45 degrees (not while the
       shield burns: those frames are the level pose) */
    {
        float y = sh_feet(e) - SH_MUZZLE_DY, dy = E->phcy - y, dx = c->facing ? E->phcx - (b->x + SH_MUZZLE_DX) : b->x - SH_MUZZLE_DX - E->phcx;
        e->aim_down = e->st != 1 && !behind && p->state != CS_DEAD && dy > 32.0f && dx > dy * 0.5f && dx < dy * 2.5f;
    }
    /* the rifle: level shots at 0.2 s cooldown pace, a long pause between them; not while the shield burns */
    bool on_screen = b->x > cam_x + 16.0f && b->x < cam_x + sw - 16.0f;
    if (on_screen && !behind && e->st != 1 && p->state != CS_DEAD && (e->t0 -= dt) <= 0) {
        bool down = e->aim_down;
        float x = b->x + (c->facing ? 1 : -1) * (down ? SH_DOWN_DX : SH_MUZZLE_DX), y = sh_feet(e) - (down ? SH_DOWN_DY : SH_MUZZLE_DY);
        int layer = e->on_deck && E->front_layer >= 0 ? E->front_layer : e->layer;   /* over the cabin wall */
        AnimDef fl = { 0, 0, 3, 0, 0.05f, 0 };
        Effect *ef = effects_spawn(fx, 0xD85FB68A, layer, &fl, x, y, 8, 8, (c->facing ? 0 : 3.1415927f) + (down ? (c->facing ? -0.7853982f : 0.7853982f) : 0));
        if (ef) { ef->follow_x = &b->x; ef->follow_y = &b->y; ef->fx0 = b->x; ef->fy0 = b->y; }
        bullets_spawn(eb, BK_ENEMY, layer, x, y, down ? (c->facing ? AIM_DR : AIM_DL) : c->facing ? AIM_R : AIM_L, 200.0f);
        sfx_play(7, 0);
        e->t0 = 1.4f + rnd(40) * 0.02f - E->difficulty * 0.2f;
    }
    float bx[4], sx[4]; sh_box(e, SH_BODY, bx); sh_box(e, SH_SHIELD, sx);
    int knock = 0; bool die = false;
    /* the hero's shots: from the front (or straight down) the shield takes them first */
    for (int i = 0; i < pb->n; i++) {
        Bullet *bl = &pb->b[i];
        float r = bl->kind == BK_GRENADE ? 8.0f : 5.0f;
        float scr = bl->x - cam_x; if (scr <= 8.0f || scr >= sw) continue;
        int d = bl->dir & 7;
        bool leftward = (1u << d) & 0x83u, front = d == AIM_U || d == AIM_D || (c->facing == 1 ? leftward : !leftward && d != AIM_U);
        bool on_shield = sh_shield_up(e) && front && bl->x + r > sx[0] && bl->x - r < sx[2] && bl->y + r > sx[1] && bl->y - r < sx[3];
        bool on_body = bl->x + r > bx[0] && bl->x - r < bx[2] && bl->y + r > bx[1] && bl->y - r < bx[3];
        if (!on_shield && !on_body) continue;
        float hx = bl->x, hy = bl->y;
        pb->b[i] = pb->b[--pb->n];
        if (on_shield) {
            AnimDef a = { 0, 4, 8, 4, 0.03f, 0 };
            effects_spawn(fx, 0x8623249C, e->layer, &a, hx, hy, 8, 8, 0);
            e->flash = 0.08f;
            if (e->st == 0 && --e->hp <= 0) { e->st = 1; e->t1 = 0; sfx_play(0x11, 0); }
            else sfx_play(14, 0);
        } else {
            knock = leftward ? -1 : (d == AIM_U || d == AIM_D) ? (c->facing ? -1 : 1) : 1;
            die = true;
        }
        break;
    }
    /* contact: the hero's slide bowls him over from behind or once the shield is gone; otherwise it hurts */
    if (!die) {
        float x0 = fminf(bx[0], sh_shield_up(e) ? sx[0] : bx[0]), x1 = fmaxf(bx[2], sh_shield_up(e) ? sx[2] : bx[2]);
        if (fabsf(E->phcx - (x0 + x1) * 0.5f) <= E->phx + (x1 - x0) * 0.5f && fabsf(E->phcy - (bx[1] + bx[3]) * 0.5f) <= E->phy + (bx[3] - bx[1]) * 0.5f) {
            if (p->state == CS_SLIDE && (behind || !sh_shield_up(e))) { die = true; knock = p->facing == 0 ? -1 : 1; }
            else if (!(p->flags & CF_HIT) && p->state != CS_DEAD) player_damage(pl, c->facing == 0 ? 0 : 4, 1);
        }
    }
    if (b->y - b->hy > E->death_floor) die = true;
    if (die) {
        c->state = CS_DEAD; b->vx = knock * 102.0f;
        if (e->st == 1) e->st = sh_shield_up(e) ? 0 : 2;   /* the death set with or without the panel */
        sfx_play(5, 0); sfx_play(6, 3);
        kill(E, e, 0x19f);
    }
}

static void draw_shield(const Enemy *e, float cam_x, float cam_y)
{
    static Sprite *spr;
    if (!spr) spr = sprite_get(SHIELD_SNIPER_SPRITE);
    if (!spr) return;
    const Character *c = &e->ch;
    int frame;
    if (e->dying) { int k = (int)((0.415f - e->death_t) / SH_DEATH_FRAME); frame = (e->aim_down ? (e->st == 0 ? 29 : 23) : (e->st == 0 ? 14 : 8)) + (k < 0 ? 0 : k > 5 ? 5 : k); }
    else if (e->st == 1) { int k = 1 + (int)(e->t1 / SH_BREAK_FRAME); frame = k > 7 ? 7 : k; }
    else if (e->aim_down) frame = e->st == 2 ? 21 : 20;
    else frame = e->st == 2 ? 7 : 0;
    float x = floorf(c->body.x - cam_x) - (c->facing ? SH_AX : 63.0f - SH_AX), y = floorf(sh_feet(e) - cam_y) - SH_FEET;
    bool flash = e->flash > 0 && !e->dying;
    if (flash) SDL_SetTextureColorMod(spr->tex, 255, 170, 170);
    /* in a tower cabin the shield's foot (it reaches 5 px below his feet) would show through the gaps under the
       front wall's bottom beam: nothing below the feet */
    SDL_Renderer *ren = e->on_deck ? SDL_GetRendererFromTexture(spr->tex) : NULL;
    SDL_Rect clip = { -64, -64, 4096, (int)(y + SH_FEET) + 64 + 1 };
    if (ren) SDL_SetRenderClipRect(ren, &clip);
    sprite_draw(spr, frame, x, y, c->facing == 0);
    if (ren) SDL_SetRenderClipRect(ren, NULL);
    if (flash) SDL_SetTextureColorMod(spr->tex, 255, 255, 255);
}

/* stage 4, after the cabin walls: a tower sniper's rifle aimed down reaches over his cabin's front wall */
void enemies_draw_front(const Enemies *E, float cam_x, float cam_y)
{
    static Sprite *spr;
    if (!spr) spr = sprite_get(SHIELD_SNIPER_SPRITE);
    if (!spr) return;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &E->e[i];
        if (e->cls != EC_SHIELD || !e->on_deck || !e->aim_down || e->dying) continue;
        const Character *c = &e->ch;
        float x = floorf(c->body.x - cam_x) - (c->facing ? SH_AX : 63.0f - SH_AX), y = floorf(sh_feet(e) - cam_y) - SH_FEET;
        bool flash = e->flash > 0;
        if (flash) SDL_SetTextureColorMod(spr->tex, 255, 170, 170);
        sprite_draw(spr, 22, x, y, c->facing == 0);
        if (flash) SDL_SetTextureColorMod(spr->tex, 255, 255, 255);
    }
}

static void update_generic(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, float cam_x, int sw, float dt)
{
    /* placeholder for classes not yet ported: stand, take hits like a walker */
    Character *c = &e->ch;
    character_sync_ground(c);
    c->aim = c->facing ? AIM_R : AIM_L;
    if (c->coll & COLL_DOWN) c->state = CS_AIM;
    update_walker(E, e, pl, L, W, pb, cam_x, sw, dt);
}

/* ours: a hero's power attack lands (power.c). Every Outrider on screen goes down at once, each in a blast; level 1's
 * boss loses boss_frac of its hit points, but only in its sweep across the play plane (the passes are out of reach,
 * as they are for the hero's shots). Returns how many went down. */
int enemies_power_strike(Enemies *E, Effects *fx, float cam_x, int sw, int sh, float boss_frac)
{
    int n = 0;
    AnimDef blast = { 0, 0, 11, 11, 0.025f, 0 };
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &E->e[i]; Character *c = &e->ch; Body *b = &c->body;
        if (!e->cls || e->dying) continue;
        if (b->x < cam_x - 16.0f || b->x > cam_x + sw + 16.0f || b->y < -48.0f || b->y > sh + 48.0f) continue;
        switch (e->cls) {
        case EC_WALKER: case EC_GRUNT: case EC_GRUNT_B: case EC_SNIPER: case EC_SNIPER_B: case EC_KNEELER: case EC_KNEELER_B:
        case EC_STALKER: case EC_SHIELD:
            if (e->cls == EC_SHIELD) { if (e->dormant) continue; if (e->st == 1) e->st = 2; }   /* the death set without the panel */
            c->state = CS_DEAD; b->vx = (b->x < E->px ? -1.0f : 1.0f) * 102.0f;
            kill(E, e, 0x19f); n++;
            effects_spawn(fx, 0x9C861FF3, e->layer, &blast, b->x, b->y - 20.0f, 32, 32, 0);
            break;
        case EC_HORSEBOSS: {
            if (e->variant == 99 || c->state != CS_WALK) break;
            int dmg = (int)ceilf((SDL_getenv("SABER_BOSSHP") ? atoi(SDL_getenv("SABER_BOSSHP")) : 0x42) * boss_frac);
            for (int k = 0; k < 6; k++) effects_spawn(fx, 0x9C861FF3, e->layer, &blast, b->x - 50.0f + rand() % 100, b->y - 40.0f + rand() % 50, 32, 32, 0);
            if ((e->hp -= dmg) <= 0) {
                e->hp = 0; kill(E, e, 0xed8); c->state = CS_DEAD; E->boss_phase = 1; c->flags &= ~CF_SHOOT;
                if (e->link >= 0 && E->e[e->link].cls) { E->e[e->link].cls = 0; E->count--; e->link = -1; }
            } else { c->flags |= CF_HIT; c->hit_t = 4.0f; }
            n++;
            break; }
        default: break;
        }
    }
    if (n) { sfx_play(5, 0); sfx_play(6, 4); }
    return n;
}

void enemies_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx,
                    float cam_x, int sw, int sh, float dt)
{
    Character *p = &pl->ch;
    E->px = p->body.x; E->py = p->body.y;
    const HurtBox *ph = &p->hurt[p->anim < CHAR_MAX_ANIMS ? p->anim : 0];
    E->phcx = p->body.x + ph->ox; E->phcy = p->body.y + ph->oy; E->phx = ph->hw; E->phy = ph->hh;
    E->death_floor = L->height; E->dt = dt;
    E->tick++;
    spawner_update(E, pl, L, W, cam_x, sw, sh, dt);
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &E->e[i];
        if (!e->cls) continue;
        if (e->dying) {
            e->death_t -= dt;
            /* FUN_0041f240: a dying entity gets no class update at all (its death pose was set on the kill frame),
             * only the timer, physics and animation; syncing the ground would reset the state to idle */
            if (e->cls == EC_HORSEBOSS) update_boss(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt);
            if (e->death_t <= 0) { int was = e->cls; e->cls = 0; E->count--; if (was == EC_BUGGY) convoy_check(E); if (was == EC_HORSEBOSS) E->boss_done = true; continue; }
        } else {
            switch (e->cls) {
            case EC_WALKER: update_walker(E, e, pl, L, W, pb, cam_x, sw, dt); break;
            case EC_GRUNT: case EC_GRUNT_B: update_grunt(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_SNIPER: case EC_SNIPER_B: update_sniper(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_KNEELER: case EC_KNEELER_B: case EC_END: update_kneeler(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_BUGGY: update_horse(E, e, pl, cam_x, sw, dt); break;
            case EC_CUTSCENE: update_cutscene_outrider(E, e, cam_x, sw, dt); break;
            case EC_SHIELD: update_shield(E, e, pl, pb, eb, fx, cam_x, sw, dt); break;
            case EC_STALKER: update_stalker(E, e, pl, pb, eb, fx, cam_x, sw, dt); break;
            case EC_HORSEBOSS: if (e->variant == 99) update_boss_rider(E, e, eb, cam_x, sw, dt); else update_boss(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_STAMPEDE: case EC_STAMPEDE + 1: case EC_STAMPEDE + 2: case EC_STAMPEDE + 3: update_stampede(E, e, cam_x, sw); break;
            case EC_PROP: case EC_PROP + 1: case EC_PROP + 2: case EC_PROP + 3: case EC_PROP + 4: case EC_PROP + 5:
            case EC_PROP + 6: case EC_PROP + 7: case EC_PROP + 8: case EC_PROP + 9: case EC_PROP + 10: case EC_PROP + 11:
                update_prop(E, e, cam_x, sw); break;
            default: update_generic(E, e, pl, L, W, pb, cam_x, sw, dt); break;
            }
        }
        physics_step(W, L, &e->ch.body, dt);
        character_animate(&e->ch, dt);
        if (SDL_getenv("SABER_TRACE") && e->cls < 8 && !e->dying && ((E->tick % 60) == 0 || (SDL_getenv("SABER_TRACE")[0] == '2' && (e->ch.body.x < cam_x + 40.0f || e->ch.body.x > cam_x + 380.0f))))
            fprintf(stderr, "enemy[%d] cls=%d type=%d st=%d face=%d pos=%.1f,%.0f hx=%.0f ox=%.0f fl=%x v=%.0f,%.0f coll=%x speed=%g gun=%d cd=%.2f cam=%.0f\n", (int)(e - E->e), e->cls, e->type, e->ch.state, e->ch.facing, e->ch.body.x, e->ch.body.y, e->ch.body.hx, e->ch.body.ox, e->ch.body.flags, e->ch.body.vx, e->ch.body.vy, e->ch.coll, e->ch.speed, e->gun_alive, e->gun_cd, cam_x);
        if (SDL_getenv("SABER_TRACE") && e->cls < 8 && !e->dying) {   /* debug: humanoid that wants to move but does not */
            if (fabsf(e->ch.body.x - e->dbg_x) < 0.5f && e->ch.body.vx != 0) { if (++e->dbg_still == 120) fprintf(stderr, "stuck? cls=%d state=%d pos=%.0f,%.0f vx=%.0f coll=%x cam=%.0f\n", e->cls, e->ch.state, e->ch.body.x, e->ch.body.y, e->ch.body.vx, e->ch.coll, cam_x); }
            else e->dbg_still = 0;
            e->dbg_x = e->ch.body.x;
        }
    }
}

void enemies_draw(const Enemies *E, int layer, float cam_x, float cam_y)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &E->e[i];
        if (!e->cls || e->layer != layer) continue;
        if (e->cls == EC_SHIELD) draw_shield(e, cam_x, cam_y);
        else character_draw(&e->ch, cam_x, cam_y);
    }
}

/* FUN_0041a960 against the player's hurtbox (called from PlayerControls) */
void player_check_enemy_bullets(Player *pl, Bullets *eb, Effects *fx, float cam_x, int sw, int sh)
{
    Character *c = &pl->ch;
    if ((c->flags & CF_HIT) || c->state == CS_DEAD) return;
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float cx = c->body.x + h->ox, cy = c->body.y + h->oy;
    for (int i = 0; i < eb->n; i++) {
        Bullet *bl = &eb->b[i];
        float r = bl->kind == BK_GRENADE ? 8.0f : bl->dark ? 3.0f : 5.0f;   /* Dark April's shots: thin, a crouch ducks them (darkapril.c SHOT_R) */
        if (fabsf(cx - bl->x) > r + h->hw || fabsf(cy - bl->y) > r + h->hh) continue;
        float sx = bl->x - cam_x, sy = bl->y; if (sx <= 8.0f || sx >= sw || sy <= 4.0f || sy >= sh - 4.0f) continue;
        int d = bl->dir;
        if (bl->kind == BK_GRENADE) { AnimDef a = { 0, 0, 12, 12, 0.0666667f, 0 }; effects_spawn(fx, 0x5B5EBBA3, 11, &a, bl->x, bl->y, 20, 28, 0); }
        eb->b[i] = eb->b[--eb->n];
        player_damage(pl, d, 1);
        return;
    }
}
