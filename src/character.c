#include "character.h"
#include "pack.h"
#include "heroes.h"
#include "platform/render.h"
#include "platform/plat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static real rdf(const uint8_t *p) { return r_bits(rd32(p)); }
/* a diagonal aim's muzzle offset: 0.7 x the straight one (in fixed point 7/10 exactly, for whole-pixel muzzles) */
#ifdef REAL_FIXED
static real diag(real mx) { return mx * 7 / 10; }
#else
static real diag(real mx) { return 0.7f * mx; }
#endif

/* the shared tables of a character type, parsed on first use (a handful of types a game) */
#define MAX_DEFS 48
static CharDef *g_defs[MAX_DEFS]; static int g_ndefs;

static const CharDef *chardef_get(uint32_t crhc_id, const uint8_t *d)
{
    for (int i = 0; i < g_ndefs; i++) if (g_defs[i]->crhc_id == crhc_id) return g_defs[i];
    CharDef *def = g_ndefs < MAX_DEFS ? calloc(1, sizeof *def) : NULL;
    if (!def) { fprintf(stderr, "CRHC %08X: no room for its tables\n", crhc_id); return NULL; }
    def->crhc_id = crhc_id;
    for (int i = 0; i < CHAR_CRHC_ANIMS; i++) {
        const uint8_t *a = d + 0x34 + i * 0x18;
        def->anims[i].id = rd32(a); def->anims[i].first = rd32(a + 4); def->anims[i].last = rd32(a + 8);
        def->anims[i].loop = rd32(a + 12); def->anims[i].frame_time = rdf(a + 16); def->anims[i].flags = rd32(a + 20);
        const uint8_t *h = d + 0x52c + i * 16;
        def->hurt[i].ox = rdf(h); def->hurt[i].oy = rdf(h + 4); def->hurt[i].hw = rdf(h + 8); def->hurt[i].hh = rdf(h + 12);
        def->muzzle[i][0] = rdf(d + 0x87c + i * 8); def->muzzle[i][1] = rdf(d + 0x880 + i * 8);
        def->anim_flags[i] = rd32(d + 0xa24 + i * 4);
    }
    hero_patch_def(def);   /* a recreated hero's table changes (April) */
    if (plat_getenv("SABER_ANIMS"))   /* debug: dump the table */
        for (int i = 0; i < CHAR_CRHC_ANIMS; i++)
            fprintf(stderr, "crhc %08X anim %2d: cells %d-%d loop %d dt %s flags %x muzzle %s,%s hurt %s,%s %sx%s\n", crhc_id, i, def->anims[i].first,
                    def->anims[i].last, def->anims[i].loop, RS(def->anims[i].frame_time, 3), def->anims[i].flags, RS(def->muzzle[i][0], 0), RS(def->muzzle[i][1], 0),
                    RS(def->hurt[i].ox, 0), RS(def->hurt[i].oy, 0), RS(def->hurt[i].hw, 0), RS(def->hurt[i].hh, 0));
    g_defs[g_ndefs++] = def;
    return def;
}

bool character_init(Character *c, uint32_t crhc_id, bool enemy)
{
    memset(c, 0, sizeof *c);
    const PackEntry *e = packs_find(crhc_id);
    if (!e || memcmp(e->data, "CRHC", 4)) { fprintf(stderr, "CRHC %08X not found\n", crhc_id); return false; }
    const uint8_t *d = e->data;
    if (!(c->def = chardef_get(crhc_id, d))) return false;
    c->crhc_id = crhc_id; c->sprite_id = rd32(d + 4);
    c->origin_x = rdf(d + 0x08); c->origin_y = rdf(d + 0x0c);
    c->speed = rdf(d + 0x10); c->slide_speed = rdf(d + 0x14); c->slide_time = rdf(d + 0x18);
    c->jump_vel = rdf(d + 0x1c); c->alert_time = rdf(d + 0x20);
    c->box_ox = rdf(d + 0x24); c->box_oy = rdf(d + 0x28); c->box_hx = rdf(d + 0x2c); c->box_hy = rdf(d + 0x30);
    c->hp_max = rd32(d + 0xadc);
    { const PackEntry *se = packs_peek(c->sprite_id); if (se && se->type == RES_SPRITE) c->spr = sprite_get(c->sprite_id); else c->cb = cblock_get(c->sprite_id); }
    static const int8_t fireball_bob[8] = { 0, 0, 1, 0, 0, 1 };
    memcpy(c->torso_bob, fireball_bob, sizeof c->torso_bob);
    c->bored_anim[0] = c->bored_anim[1] = -1; c->walk_aim_ov = WALK_AIM_ALL; c->ov_sync = false;
    hero_apply(c);                 /* player heroes only: recreated sheets (April) replace the pack's cblock + patch the table */
    c->body.ox = c->box_ox; c->body.oy = c->box_oy; c->body.hx = c->box_hx; c->body.hy = c->box_hy;
    character_reset(c, enemy);
    c->anim = 0xff; c->overlay = 0;
    return true;
}

/* FUN_0041ba30 */
void character_reset(Character *c, bool enemy)
{
    c->state = CS_FALL; c->coll = 0; c->facing = 1; c->aim = AIM_R;
    c->slide_t = 0; c->drop_target_y = R(-10000);
    c->flags = enemy ? 0x102 : 0x002;
    c->alert_t = c->alert_time;
    c->hit_t = 0; c->base_ox = c->base_oy = 0; c->muzzle_x = c->muzzle_y = 0;
}

/* FUN_0041ba90: pull collision result from physics into the state machine */
void character_sync_ground(Character *c)
{
    c->coll = c->body.coll;
    if (c->coll & COLL_DOWN) {
        if (c->state == CS_IDLE) {
            if (c->flags & CF_SHOOT) c->alert_t = c->alert_time;
        } else {
            c->alert_t = c->alert_time;
            if (c->state != CS_SLIDE) c->state = CS_IDLE;
        }
        if (c->body.ground_tile == 4) c->flags = (c->flags & ~3u) | CF_ON_ONEWAY;
        else c->flags &= ~(3u | CF_ON_ONEWAY);
        c->flags &= ~(CF_DROP_REQ | CF_SPAWN_FALL);
        return;
    }
    if ((c->flags & 3) == 0) c->state = CS_AIR;
    else if (c->state != CS_AIR) c->state = CS_FALL;
    c->flags &= ~(CF_CROUCH | CF_DROP_REQ | CF_ON_ONEWAY);
}

/* FUN_0041b710: switch animation, update hurtbox + muzzle offset */
void character_set_anim(Character *c, int anim)
{
    if (anim < 0 || anim >= CHAR_MAX_ANIMS) return;
    if (c->flags & CF_HIT) {
        c->hit_t -= R(1);
        if (c->hit_t < R(0.01)) { c->flags &= ~(CF_HIT | CF_HIT_ALT); c->hit_t = 0; }
    }
    if (c->anim != anim) {
        c->anim = (uint8_t)anim;
        c->frame = c->def->anims[anim].first; c->anim_t = 0;
    }
    uint32_t af = c->def->anim_flags[anim];
    if (af & 1) {
        real mx = c->def->muzzle[anim][0], my = c->def->muzzle[anim][1], x, y;
        if (af & 2) {
            real d = diag(mx);
            switch (c->aim) {
            case AIM_L:  x = -mx;  y = 0;    break;
            case AIM_UL: x = -d;   y = -d;   break;
            case AIM_U:  x = 0;    y = -mx;  break;
            case AIM_UR: x = d;    y = -d;   break;
            case AIM_DR: x = d;    y = d;    break;
            case AIM_D:  x = 0;    y = mx;   break;
            case AIM_DL: x = -d;   y = d;    break;
            default:     x = mx;   y = 0;    break;
            }
        } else { x = mx; y = 0; }
        c->muzzle_x = x + c->base_ox; c->muzzle_y = y + my + c->base_oy;
    }
}

/* FUN_0041b570: torso overlay animation (anim 0 = blank cell) + muzzle from the overlay's table entry */
void character_set_overlay(Character *c, int anim)
{
    if (anim < 0 || anim >= CHAR_MAX_ANIMS) return;
    if (c->overlay != anim) {
        const AnimDef *o = &c->def->anims[anim], *a = &c->def->anims[c->anim];
        c->overlay = (uint8_t)anim; c->ov_frame = o->first; c->ov_t = 0;
        if (c->ov_sync && o->last - o->first == a->last - a->first && o->frame_time == a->frame_time) {
            c->ov_frame = o->first + (c->frame - a->first); c->ov_t = c->anim_t;
        }
    }
    uint32_t af = c->def->anim_flags[anim];
    if (af & 1) {
        real mx = c->def->muzzle[anim][0], my = c->def->muzzle[anim][1], x, y;
        if (af & 2) {
            real d = diag(mx);
            switch (c->aim) {
            case AIM_L:  x = -mx;  y = 0;    break;
            case AIM_UL: x = -d;   y = -d;   break;
            case AIM_U:  x = 0;    y = -mx;  break;
            case AIM_UR: x = d;    y = -d;   break;
            case AIM_DR: x = d;    y = d;    break;
            case AIM_D:  x = 0;    y = mx;   break;
            case AIM_DL: x = -d;   y = d;    break;
            default:     x = mx;   y = 0;    break;
            }
        } else { x = mx; y = 0; }
        c->muzzle_x = x + c->base_ox; c->muzzle_y = y + my + c->base_oy;
    }
}

/* SpriteAnimation::update for one entity */
void character_animate(Character *c, real dt)
{
    if (c->anim >= CHAR_MAX_ANIMS) return;
    const AnimDef *a = &c->def->anims[c->anim];
    if (a->frame_time <= 0) return;
    c->anim_t += dt;
    while (c->anim_t >= a->frame_time) {
        c->anim_t -= a->frame_time;
        c->frame = (c->frame < a->last) ? c->frame + 1 : a->loop;
    }
    const AnimDef *o = &c->def->anims[c->overlay];
    if (o->frame_time > 0) {
        c->ov_t += dt;
        while (c->ov_t >= o->frame_time) { c->ov_t -= o->frame_time; c->ov_frame = (c->ov_frame < o->last) ? c->ov_frame + 1 : o->loop; }
    }
}

/* FUN_0041bc40: state -> animation + horizontal velocity, drop-through handling */
void character_resolve(Character *c, real dt)
{
    Body *b = &c->body;
    real vx = 0;
    int L = c->facing == 0;
    bool shoot = (c->flags & CF_SHOOT) != 0;
    int anim = -1;
    switch (c->state) {
    case CS_IDLE:
        if (shoot) anim = L ? 12 : 15;
        else if (c->alert_t > 0) { anim = L ? 4 : 7; c->alert_t -= dt; }
        else anim = L ? 1 : 2;
        c->flags &= ~CF_CROUCH;
        break;
    case CS_WALK:
        anim = L ? (shoot ? 0x1c : 0x24) : (shoot ? 0x1f : 0x25);
        vx = L ? -c->speed : c->speed;
        c->flags &= ~CF_CROUCH;
        break;
    case CS_AIR:
        if (c->flags & CF_SPAWN_FALL) { anim = (c->flags & CF_IN_JUMP) ? (L ? 0x2e : 0x2f) : 0x34; vx = 0; }
        else { vx = L ? -c->speed : c->speed; anim = (c->flags & CF_IN_JUMP) ? (L ? 0x2e : 0x2f) : (L ? 0x30 : 0x31); }
        if (c->coll & (L ? COLL_LEFT : COLL_RIGHT)) c->state = CS_FALL;
        break;
    case CS_JUMP:
        if (c->coll & COLL_DOWN) {
            b->vy += c->jump_vel;
            anim = L ? 0x2e : 0x2f;
            c->flags |= CF_IN_JUMP;
        }
        goto keep_vx;
    case CS_DROP:
        if (c->coll & COLL_DOWN) {
            c->flags |= CF_DROPPING;
            c->drop_target_y = b->y + R(20);
            vx = 0;
        }
        goto keep_vx;
    case CS_FALL:
        if (c->flags & CF_IN_JUMP) anim = L ? 0x2e : 0x2f;
        else anim = (c->flags & CF_SPAWN_FALL) ? 0x34 : (L ? 0x30 : 0x31);
        break;
    case CS_CROUCH:
        anim = L ? (shoot ? 0x2a : 0x28) : (shoot ? 0x2b : 0x29);
        break;
    case CS_SLIDE:
        vx = r_mul(r_div(c->slide_t, c->slide_time), c->slide_speed);
        if (L) vx = -vx;
        anim = L ? 0x2c : 0x2d;
        c->slide_t -= dt;
        if (c->slide_t <= 0) c->state = CS_IDLE;
        break;
    case CS_AIM: {
        static const uint8_t tab[8][2] = { {4,12},{5,13},{10,18},{8,16},{7,15},{9,17},{11,19},{6,14} };
        if (c->aim < 8) anim = tab[c->aim][shoot ? 1 : 0];
        break; }
    case CS_DEAD:
        anim = L ? 0x32 : 0x33;
        break;
    }
    if (anim >= 0) character_set_anim(c, anim);
    b->vx = vx;
keep_vx:
    /* drop-through one-way platforms: ignore floors until below the target */
    if (b->y < c->drop_target_y) { b->flags |= PHYS_IGNORE_DOWN; return; }
    b->flags &= ~PHYS_IGNORE_DOWN;
    c->drop_target_y = R(-10000);
}

/* anim flags low byte = sprite draw flags: 1 mirror X, 2 mirror Y, 8 draw whole cblock frame (else one cell) */
static void draw_cell(const Character *c, int idx, int aflags, real x, real y)
{
    bool flip = (aflags & 1) != 0;
    if (c->spr) { sprite_draw(c->spr, idx, x, y, flip); return; }
    if (!c->cb) return;
    if (aflags & 8) { if (idx >= 0 && idx < c->cb->frames) cblock_draw_frame(c->cb, idx, x, y, flip); return; }
    if (idx < 0 || idx >= cblock_ncells(c->cb)) return;
    uint16_t t = c->cb->cells[idx];
    if (t == 0xFFFF) return;
    cblock_draw_tile(c->cb, t, x, y, flip);
}

void character_draw(const Character *c, real cam_x, real cam_y)
{
    if (c->anim >= CHAR_MAX_ANIMS) return;
    real x = r_floorr(c->body.x - c->origin_x - cam_x), y = r_floorr(c->body.y - c->origin_y - cam_y);
    if ((c->flags & CF_HIT) && c->hit_t > R(0.01) && (plat_ticks_ms() / 16 & 2)) return;   /* invulnerability blink (effect flag 0x10 every other 2 frames) */
    draw_cell(c, c->frame, (int)(c->def->anims[c->anim].flags & 0xff), x, y);
    /* the torso is its own sprite object placed at the base offset (up/down aims, 1 px walk bob) */
    real oy = c->base_oy;
    if (c->walk_bob) { int k = c->frame - (int)c->def->anims[c->anim].first; if (k >= 0 && k < 8) oy += r_int(c->torso_bob[k]); }   /* the hip drops with the legs frame */
    if (c->overlay) draw_cell(c, c->ov_frame, (int)(c->def->anims[c->overlay].flags & 0xff), x + c->base_ox, y + oy);
}

/* FUN_0041c530: player state -> legs anim, torso overlay anim, muzzle base offset, horizontal velocity */
void player_resolve(Character *c, real dt)
{
    Body *b = &c->body;
    bool L = c->facing == 0;
    bool shoot = (c->flags & CF_SHOOT) != 0;
    int aim = c->aim;
    int body = -1, ov = 0;
    real vx = 0;
    c->base_ox = c->base_oy = 0;
#define UPBASE()   do { c->base_ox = L ? R(4) : R(-4); c->base_oy = R(-29); } while (0)
#define DOWNBASE() do { c->base_ox = L ? R(4) : R(-4); c->base_oy = R(-8); } while (0)
    if (c->state != CS_IDLE) c->idle_t = 0;
    switch (c->state) {
    case CS_IDLE:
        if (shoot) { body = L ? 12 : 15; c->idle_t = 0; }
        else if (c->alert_t > 0) { body = L ? 4 : 7; c->idle_t = 0; }
        else {
            body = L ? 1 : 2;
            c->idle_t += dt;
            if (c->bored_anim[0] >= 0 && c->idle_t >= c->bored_time) {   /* play the bored animation once, then sway again */
                int bored = c->bored_anim[L ? 0 : 1];
                if (c->anim == bored && c->frame >= c->def->anims[bored].last) c->idle_t = 0;
                else body = bored;
            }
        }
        c->flags &= ~CF_CROUCH;
        c->alert_t -= dt;
        break;
    case CS_WALK:
        body = L ? 0x24 : 0x25; vx = L ? -c->speed : c->speed;
        if (c->walk_aim_ov == WALK_AIM_NONE || (c->walk_aim_ov == WALK_AIM_DIAG && aim != AIM_UL && aim != AIM_DL && aim != AIM_UR && aim != AIM_DR)) ov = L ? 0x26 : 0x27;
        else if (L) ov = shoot ? (aim == 1 ? 0x1d : aim == 7 ? 0x1e : 0x1c) : (aim == 1 ? 0x15 : aim == 7 ? 0x16 : 0x26);
        else   ov = shoot ? (aim == 3 ? 0x20 : aim == 5 ? 0x21 : 0x1f) : (aim == 3 ? 0x18 : aim == 5 ? 0x19 : 0x27);
        c->flags &= ~CF_CROUCH;
        break;
    case CS_AIR:
    case CS_FALL: {
        vx = c->state == CS_AIR ? (L ? -c->speed : c->speed) : 0;
        if (c->flags & CF_IN_JUMP) { body = L ? 0x2e : 0x2f; ov = 0; }
        else {
            body = L ? 0x30 : 0x31;
            if (aim == 2) { UPBASE(); ov = shoot ? 0x22 : 0x1a; }
            else if (aim == 6) { DOWNBASE(); ov = shoot ? 0x23 : 0x1b; }
            else if (L) ov = shoot ? (aim == 1 ? 0x1d : aim == 7 ? 0x1e : 0x1c) : (aim == 1 ? 0x15 : aim == 7 ? 0x16 : 0x14);
            else ov = shoot ? (aim == 3 ? 0x20 : aim == 5 ? 0x21 : 0x1f) : (aim == 3 ? 0x18 : aim == 5 ? 0x19 : 0x17);
        }
        if (c->state == CS_AIR && (c->coll & (L ? COLL_LEFT : COLL_RIGHT))) c->state = CS_FALL;
        break; }
    case CS_JUMP:
        if (c->coll & COLL_DOWN) { b->vy += c->jump_vel; body = L ? 0x2e : 0x2f; ov = 0; c->flags |= CF_IN_JUMP; }
        goto keep_vx;
    case CS_DROP:
        if (c->coll & COLL_DOWN) { c->flags |= CF_DROPPING; c->drop_target_y = b->y + R(20); vx = 0; }
        goto keep_vx;
    case CS_CROUCH:
        body = L ? (shoot ? 0x2a : 0x28) : (shoot ? 0x2b : 0x29); ov = 0;
        break;
    case CS_SLIDE:
        vx = r_mul(r_div(c->slide_t, c->slide_time), c->slide_speed); if (L) vx = -vx;
        body = L ? 0x2c : 0x2d; ov = 0;
        c->slide_t -= dt; if (c->slide_t <= 0) c->state = CS_IDLE;
        break;
    case CS_AIM: {
        static const uint8_t tab[8][2] = { {4,12},{5,13},{0,0},{8,16},{7,15},{9,17},{0,0},{6,14} };
        if (aim == 2) { c->base_oy = R(-30); body = 3; ov = shoot ? 0x22 : 0x1a; }
        else if (aim == 6) { c->base_oy = R(-9); body = 3; ov = shoot ? 0x23 : 0x1b; }
        else { body = tab[aim & 7][shoot ? 1 : 0]; ov = 0; }
        break; }
    case CS_DEAD:
        body = L ? 0x32 : 0x33; ov = 0;
        break;
    }
    if (body >= 0) character_set_anim(c, body);
    character_set_overlay(c, ov);
    /* walk cycle: legs cells 2 and 5 sit 1 px lower, the torso follows (set after the muzzle, so it only moves the art) */
    c->walk_bob = c->state == CS_WALK;
    b->vx = vx;
keep_vx:
    if (b->y < c->drop_target_y) { b->flags |= PHYS_IGNORE_DOWN; return; }
    b->flags &= ~PHYS_IGNORE_DOWN;
    c->drop_target_y = R(-10000);
#undef UPBASE
#undef DOWNBASE
}

/* ---- input helpers ---- */
void character_move_left(Character *c, int vdir)   /* FUN_0041d740, vdir 2 up / 6 down / 0 */
{
    if (c->state == CS_SLIDE) return;
    c->facing = 0;
    if (vdir == 2) c->aim = AIM_UL;
    else if (vdir == 6) {
        c->aim = AIM_DL;
        if (c->coll & COLL_DOWN) { c->flags |= CF_CROUCH; c->slide_t = c->slide_time; c->state = CS_WALK; return; }
        c->state = CS_AIR; return;
    } else c->aim = AIM_L;
    c->state = (c->coll & COLL_DOWN) ? CS_WALK : CS_AIR;
}
void character_move_right(Character *c, int vdir)  /* FUN_0041d790 */
{
    if (c->state == CS_SLIDE) return;
    c->facing = 1;
    if (vdir == 2) c->aim = AIM_UR;
    else if (vdir == 6) {
        c->aim = AIM_DR;
        if (c->coll & COLL_DOWN) { c->flags |= CF_CROUCH; c->slide_t = c->slide_time; c->state = CS_WALK; return; }
        c->state = CS_AIR; return;
    } else c->aim = AIM_R;
    c->state = (c->coll & COLL_DOWN) ? CS_WALK : CS_AIR;
}
void character_down(Character *c)                  /* FUN_0041d820 */
{
    if (c->state == CS_SLIDE) return;
    if (c->coll & COLL_DOWN) {
        c->state = CS_CROUCH;
        c->aim = c->facing ? AIM_R : AIM_L;
        if (c->flags & CF_ON_ONEWAY) { c->flags |= CF_DROP_REQ; return; }
        c->flags |= CF_CROUCH; c->slide_t = c->slide_time;
        return;
    }
    c->aim = AIM_D;
}
void character_aim(Character *c, int aim) { c->aim = (uint8_t)aim; }                 /* FUN_0041d900 */
void character_idle_aim(Character *c) { c->aim = c->facing ? AIM_R : AIM_L; }        /* FUN_0041d910 */
void character_aim_stand(Character *c)             /* FUN_0041d880 */
{
    if (c->state == CS_SLIDE || !(c->coll & COLL_DOWN)) return;
    uint8_t a = c->aim;
    c->state = CS_AIM;
    if ((a & 0xfb) != 2)            /* not straight up/down */
        c->facing = a > 7 || ((1u << a) & 0x83u) == 0;   /* left for aims 0,1,7 */
}
void character_jump(Character *c)                  /* FUN_0041d7e0 */
{
    if (!(c->coll & COLL_DOWN)) return;
    if (c->flags & CF_DROP_REQ) { c->state = CS_DROP; return; }
    if (c->flags & CF_CROUCH) { c->state = CS_SLIDE; return; }
    if (c->state != CS_SLIDE) c->state = CS_JUMP;
}
bool character_request_shoot(Character *c)         /* FUN_0041d8d0 */
{
    if (c->state == CS_SLIDE || (c->flags & CF_SPAWN_FALL)) return false;
    c->flags |= CF_SHOOT;
    return true;
}
