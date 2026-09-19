#include "enemies.h"
#include "pack.h"
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

static uint32_t crhc_for_type(int t) { return (t >= 2 && t <= 29) ? TYPE_CRHC[t - 2] : 0x02A38AFB; }
static int rnd(int n) { return n > 0 ? rand() % n : 0; }   /* FUN_0040cf30(0, n) -> [0,n) */

void enemies_reset(Enemies *E) { memset(E, 0, sizeof *E); }

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
    t->remaining = o->loops;
    for (int i = 0; i < o->n_wp && i < 8; i++) { t->wp[i][0] = o->wp[i][0]; t->wp[i][1] = o->wp[i][1]; t->nwp++; }
    if (t->nwp == 0) { t->wp[0][0] = o->x; t->wp[0][1] = o->y; t->nwp = 1; }
}

static Enemy *alloc_slot(Enemies *E, int cls)   /* FUN_0041f980 */
{
    for (int i = 0; i < MAX_ENEMIES; i++) if (!E->e[i].cls) { memset(&E->e[i], 0, sizeof(Enemy)); E->e[i].cls = (uint16_t)cls; E->count++; return &E->e[i]; }
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

/* Look ahead `right?` for walls/ledges; auto-jump if a jump clears the obstacle. */
static void face_and_probe(Enemy *e, bool right, const Level *L, const PhysicsWorld *W, float cam_x, int sw)
{
    Character *c = &e->ch; Body *b = &c->body;
    if (right) character_move_right(c, 4); else character_move_left(c, 0);
    if (b->x > cam_x + sw - 16.0f && right) return;   /* original: only probes when on-screen-ish */
    if (b->y <= 8.0f) return;
    Body save = *b;
    int n = 32; bool jump = false;
    Body p = save; p.vx = right ? 120.0f : -120.0f; p.vy = 0; p.coll = 0;
    uint8_t hit = 0; int k = 0;
    for (; k < n; k++) { physics_step(W, L, &p, 1.0f / 60.0f); hit = p.coll; if (hit & (COLL_LEFT | COLL_RIGHT)) break; }
    if (hit & (COLL_LEFT | COLL_RIGHT)) {
        p = save; p.vx = right ? 120.0f : -120.0f; p.vy = -250.0f; p.coll = 0;
        for (k = 0; k < n; k++) { physics_step(W, L, &p, 1.0f / 60.0f); if (p.coll & (COLL_LEFT | COLL_RIGHT)) break; }
        if (!(p.coll & (COLL_LEFT | COLL_RIGHT))) jump = true;
    }
    *b = save;
    if (jump) b->vy -= 250.0f;
}

Enemy *enemy_spawn(Enemies *E, int type, int layer, float x, float y, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh)
{
    int cls;
    switch (type) {
    case 2: case 5: cls = type == 2 ? EC_GRUNT : EC_GRUNT_B; break;
    case 6: case 7: cls = type == 6 ? EC_SNIPER : EC_SNIPER_B; break;
    case 8: case 9: cls = type == 8 ? EC_KNEELER : EC_KNEELER_B; break;
    case 10: cls = EC_HORSEBOSS; break;
    case 11: cls = EC_BUGGY; break;
    case 28: cls = EC_CUTSCENE; break;
    case 29: cls = EC_END; break;
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
    e->variant = (type == 5 || type == 7 || type == 9 || type == 29);
    if (cls == EC_GRUNT || cls == EC_GRUNT_B || cls == EC_SNIPER || cls == EC_SNIPER_B || cls == EC_KNEELER || cls == EC_KNEELER_B || cls == EC_END) e->gun_alive = true;
    if (cls == EC_WALKER || cls == EC_GRUNT || cls == EC_GRUNT_B) {
        e->dir = E->px <= b->x ? 0 : 1;
        face_and_probe(e, e->dir == 1, L, W, cam_x, sw);
        snap_to_ground(e, L, W);
        if (e->ch.flags & CF_SPAWN_FALL) e->spawn_t = 0.6666667f;
    } else if (cls >= EC_PROP && cls <= EC_STAMPEDE + 3) {
        b->x = x; b->y = y; b->vx = b->vy = 0; b->flags = 0x1f;
        e->ch.state = CS_IDLE;
        if (cls >= EC_STAMPEDE) { if (E->px <= b->x) character_move_left(&e->ch, 0); else character_move_right(&e->ch, 4); }
    } else {
        e->dir = 2;
        e->ch.facing = E->px <= b->x ? 0 : 1;
        snap_to_ground(e, L, W);
    }
    return e;
}

static void spawner_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, float cam_x, int sw, int sh, float dt)   /* FUN_00421370 */
{
    Body *pb = &pl->ch.body;
    float pcx = pb->x + pb->ox, pcy = pb->y + pb->oy;
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
            Enemy *e = enemy_spawn(E, t->type, t->layer, wx, wy, L, W, cam_x, sw, sh);
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
    if (dmg <= pl->hp) { pl->hp -= dmg; c->flags |= CF_HIT; c->hit_t = 170.0f; return true; }
    if ((hit_dir & 0xfb) != 2) {
        if (hit_dir < 8 && ((1u << hit_dir) & 0x83u)) { c->facing = 1; c->body.vx -= 300.0f; }
        else { c->facing = 0; c->body.vx += 300.0f; }
    }
    c->state = CS_DEAD;
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
            if (c->state != CS_DEAD) { if (p->facing == 0) { e->dir = 1; knock = -1; } else { e->dir = 0; knock = 1; } }
            die = true; c->state = CS_DEAD;
        } else if (!(p->flags & CF_HIT) && p->state != CS_DEAD) {
            player_damage(pl, e->dir == 0 ? 0 : 4, 1);
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
                if (d < 8 && ((1u << d) & 0x83u)) { e->dir = 1; knock = -1; } else { e->dir = 0; knock = 1; }
            }
            die = true; c->state = CS_DEAD;
            break;
        }
    }
resolve:
    if (b->y - b->hy > E->death_floor) { die = true; c->state = CS_DEAD; }
    character_resolve(c, dt);
    if (e->gun_alive) { e->gun_cd -= dt; if (e->gun_cd <= 0) { if (e->gun_cd < 0) e->gun_cd = 0; c->flags &= ~CF_SHOOT; } }
    if (knock) b->vx += knock * 102.0f;
    if (die) kill(E, e, 0x19f);
}

static void update_walker(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, float cam_x, int sw, float dt)   /* FUN_004144d0 */
{
    Character *c = &e->ch; Body *b = &c->body;
    (void)L;
    character_sync_ground(c);
    bool offscreen = false;
    if (e->dir == 1) {
        if (!(c->coll & COLL_RIGHT)) { character_move_right(c, 4); offscreen = b->x - b->hx > cam_x + sw; }
        else character_move_left(c, 0);
    } else if (e->dir == 0) {
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
                float mx = e->dir == 0 ? (e->variant ? -29.0f : -26.0f) : (e->variant ? 29.0f : 26.0f);
                if (e->gun_alive && e->gun_cd <= 0.0f) {
                    float x = b->x + mx, y = b->y + c->muzzle_y;
                    AnimDef fl = { 0, 0, 3, 0, 0.05f, 0 };
                    Effect *ef = effects_spawn(fx, 0xD85FB68A, e->layer, &fl, x, y, 8, 8, c->aim == AIM_L ? 3.1415927f : 0);
                    if (ef) { ef->follow_x = &b->x; ef->follow_y = &b->y; ef->fx0 = b->x; ef->fy0 = b->y; }
                    bullets_spawn(eb, BK_ENEMY, e->layer, x, y, c->aim & 7, 166.0f);
                    e->gun_cd = 0.2f;
                }
            } else if (c->speed > 0.00024f) {
                c->speed = 120.0f; e->gun_alive = false;
                if (e->dir == 0) character_move_right(c, 4); else character_move_left(c, 0);
            }
        }
    } else if (e->dir == 0) {
        if (c->coll & COLL_LEFT) {
            if (W->world_min_x <= b->x - b->hx - 1.0f) { e->gun_alive = false; character_move_right(c, 4); }
            else { b->flags = PHYS_IGNORE_LEFT; character_move_left(c, 0); }
        } else {
            character_move_left(c, 0);
            if (b->x + b->hx < W->world_min_x) offscreen = true;
            else if (c->state != CS_AIR && E->px + quarter < b->x && b->x < cam_x + sw - quarter && rnd(100) >= 0x60 && e->gun_alive
                     && character_request_shoot(c)) c->speed = 0;
        }
    } else if (e->dir == 1) {
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
                float th = below ? 24.0f : 48.0f;
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
        e->dir = (c->aim < 8 && ((1u << c->aim) & 0x83u)) ? 0 : 1;
        c->facing = e->dir;
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

static void update_generic(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, float cam_x, int sw, float dt)
{
    /* placeholder for classes not yet ported: stand, take hits like a walker */
    Character *c = &e->ch;
    character_sync_ground(c);
    c->aim = c->facing ? AIM_R : AIM_L;
    if (c->coll & COLL_DOWN) c->state = CS_AIM;
    update_walker(E, e, pl, L, W, pb, cam_x, sw, dt);
}

void enemies_update(Enemies *E, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, Bullets *eb, Effects *fx,
                    float cam_x, int sw, int sh, float dt)
{
    Character *p = &pl->ch;
    E->px = p->body.x; E->py = p->body.y;
    const HurtBox *ph = &p->hurt[p->anim < CHAR_MAX_ANIMS ? p->anim : 0];
    E->phcx = p->body.x + ph->ox; E->phcy = p->body.y + ph->oy; E->phx = ph->hw; E->phy = ph->hh;
    E->death_floor = L->height; E->dt = dt;
    spawner_update(E, pl, L, W, cam_x, sw, sh, dt);
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &E->e[i];
        if (!e->cls) continue;
        if (e->dying) {
            e->death_t -= dt;
            character_sync_ground(&e->ch);
            character_resolve(&e->ch, dt);
            if (e->death_t <= 0) { e->cls = 0; E->count--; continue; }
        } else {
            switch (e->cls) {
            case EC_WALKER: update_walker(E, e, pl, L, W, pb, cam_x, sw, dt); break;
            case EC_GRUNT: case EC_GRUNT_B: update_grunt(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_SNIPER: case EC_SNIPER_B: update_sniper(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_KNEELER: case EC_KNEELER_B: case EC_END: update_kneeler(E, e, pl, L, W, pb, eb, fx, cam_x, sw, dt); break;
            case EC_STAMPEDE: case EC_STAMPEDE + 1: case EC_STAMPEDE + 2: case EC_STAMPEDE + 3: update_stampede(E, e, cam_x, sw); break;
            case EC_PROP: case EC_PROP + 1: case EC_PROP + 2: case EC_PROP + 3: case EC_PROP + 4: case EC_PROP + 5:
            case EC_PROP + 6: case EC_PROP + 7: case EC_PROP + 8: case EC_PROP + 9: case EC_PROP + 10: case EC_PROP + 11:
                update_prop(E, e, cam_x, sw); break;
            default: update_generic(E, e, pl, L, W, pb, cam_x, sw, dt); break;
            }
        }
        physics_step(W, L, &e->ch.body, dt);
        character_animate(&e->ch, dt);
    }
}

void enemies_draw(const Enemies *E, int layer, float cam_x, float cam_y)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &E->e[i];
        if (!e->cls || e->layer != layer) continue;
        character_draw(&e->ch, cam_x, cam_y);
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
        float r = bl->kind == BK_GRENADE ? 8.0f : 5.0f;
        if (fabsf(cx - bl->x) > r + h->hw || fabsf(cy - bl->y) > r + h->hh) continue;
        float sx = bl->x - cam_x, sy = bl->y; if (sx <= 8.0f || sx >= sw || sy <= 4.0f || sy >= sh - 4.0f) continue;
        int d = bl->dir;
        if (bl->kind == BK_GRENADE) { AnimDef a = { 0, 0, 12, 12, 0.0666667f, 0 }; effects_spawn(fx, 0x5B5EBBA3, 11, &a, bl->x, bl->y, 20, 28, 0); }
        eb->b[i] = eb->b[--eb->n];
        player_damage(pl, d, 1);
        return;
    }
}
