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
    if (cls == EC_WALKER || cls == EC_GRUNT || cls == EC_GRUNT_B) {
        e->dir = E->px <= b->x ? 0 : 1;
        face_and_probe(e, e->dir == 1, L, W, cam_x, sw);
        snap_to_ground(e, L, W);
        if (e->ch.flags & CF_SPAWN_FALL) e->spawn_t = 0.6666667f;
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

static bool player_damage(Player *pl, int hit_dir, int dmg)   /* FUN_00422a10 */
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

static void update_walker(Enemies *E, Enemy *e, Player *pl, const Level *L, const PhysicsWorld *W, Bullets *pb, float cam_x, int sw, float dt)   /* FUN_004144d0 */
{
    Character *c = &e->ch; Body *b = &c->body;
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

    int knock = 0; bool die = false;
    if ((c->flags & CF_SPAWN_FALL) && e->spawn_t > 0) {
        e->spawn_t -= dt; if (e->spawn_t < 0) e->spawn_t = 0;
        goto resolve;
    }
    /* contact with the player */
    if (hurt_overlap(c, b->x, b->y, E)) {
        Character *p = &pl->ch;
        if (p->state == CS_SLIDE) {
            if (c->state != CS_DEAD) { if (p->facing == 0) { e->dir = 1; knock = -1; } else { e->dir = 0; knock = 1; } }
            die = true; c->state = CS_DEAD;
        } else if (!(p->flags & CF_HIT) && p->state != CS_DEAD) {
            player_damage(pl, e->dir == 0 ? 0 : 4, 1);
        }
    }
    /* player bullets */
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
    if (knock) b->vx += knock * 102.0f;
    if (die || offscreen) kill(E, e, 0x19f);
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
    (void)eb; (void)fx;
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
