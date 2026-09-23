#include "darkapril.h"
#include "heroes.h"
#include "audio.h"
#include "assets.h"
#include "font.h"
#include "namehash.h"
#include "enemies.h"   /* player_damage */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CRHC_APRIL 0x79260A58

/* ---- the story ----
 * Stage 5 follows the jungle: the watchtowers' jamming was relayed from somewhere, and April traced it to a cave
 * under the mountains - an Outrider laboratory built from stolen Cavalry Command equipment. The level-1 gunship
 * guards its hall. When it falls, Ramrod reads one more lifesign next to the hero, and it reads like April.
 * Nobody - April least of all - can explain what forms there, and the scenes leave it that way.
 * Every page fits one box (4 wrapped lines with any hero's name swapped in: SABER_DLGCHECK). */
const char *const LAB_SCRIPT_INTRO =
    "<|GREEN|>\n</dialog_avatar_april2/>\nFireball, the jamming in that jungle was relayed from somewhere. I traced the signal to this cave.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nRamrod reads a whole laboratory under this mountain, built out of stolen Cavalry Command equipment.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nA hideout in a hole in the ground. Figures - those phantoms never did care for sunshine.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nThe tunnels are too narrow for Ramrod, so it's you on foot again. Find that lab and shut it down.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nGoing home keeps getting postponed, doesn't it? Fine. Lights out for this lab.\n<<>>\n";
const char *const DARK_SCRIPT_CALL =
    "<|GREEN|>\n</dialog_avatar_april2/>\nNice shooting, Fireball! That's their guard dog grounded. Hold on... Ramrod's picking up another lifesign.\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nIt's right there beside you. And it reads just like... me?\n<<>>\n";
const char *const DARK_SCRIPT_MEET =
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nApril?! What are you doing down here? You're supposed to be up on Ramrod!\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nI AM up on Ramrod, Fireball! I'm looking right at the scanner, and it says I'm standing next to you!\n<<>>\n"
    "<|PURPLE|>\n</dialog_avatar_darkapril/>\nWhatever you do, Star Sheriff... I do it too. Shall we see who does it better?\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nTwo Aprils? I can barely keep up with the one we've got!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nRamrod can't make sense of her either. A clone, a phantom, a reflection... no idea. Just be careful.\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nFireball, whatever she is, she's NOT me. Don't you dare go easy on her!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nWasn't planning on it. Sorry, April... the other April.\n<<>>\n";
const char *const DARK_SCRIPT_OUTRO =
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nShe's gone... like she was never here. April, tell me Ramrod saw that too.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nIt saw it. Now the scanner shows nothing - not a trace. I don't like this one bit.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nWhatever she was, she came out of this lab. Commander Eagle will want every console in here.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nLong as there's only ONE April on the ride home. Come on up, we're heading out.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nDeal. And April? For the record, the real one's a lot nicer.\n<<>>\n";
/* April as the hero: she meets herself */
const char *const DARK_SCRIPT_CALL_APRIL =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nNice shooting, April! That's their guard dog grounded. Hold on... Ramrod's picking up another lifesign.\n<<>>\n"
    "<|RED|>\n</dialog_avatar_saber2/>\nIt's right there beside you. That's strange... it reads exactly like yours.\n<<>>\n";
const char *const DARK_SCRIPT_MEET_APRIL =
    "<|GREEN|>\n</dialog_avatar_april2/>\nHuh?! Who's there? ...Wait. That's my hair. That's my FACE!\n<<>>\n"
    "<|PURPLE|>\n</dialog_avatar_darkapril/>\nHello, April. Nice outfit.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nNice-- that's MY outfit! Saber Rider, are you seeing this?!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nRamrod reads two of you down there. Same height, same weight... the scanner can't tell you apart.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nWell, one of you is purple. I'm rooting for the one that isn't purple.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nDon't look at me, darlin'. I ain't got an explanation for this one either.\n<<>>\n"
    "<|PURPLE|>\n</dialog_avatar_darkapril/>\nWhatever you do, I do. Whatever you know, I know. Let's find out which one of us is the copy.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nI don't know WHAT you are... but nobody copies April Eagle!\n<<>>\n";
const char *const DARK_SCRIPT_OUTRO_APRIL =
    "<|GREEN|>\n</dialog_avatar_april2/>\n...She just vanished. Saber Rider, please tell me Ramrod recorded all of that.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nEvery second of it. And none of it makes any sense. Commander Eagle is going to love this report.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nFor the record, I knew which one was you the whole time. Mostly.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nTwo Aprils in one day. Reckon I'll need a vacation after this one.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\nLet's shut this lab down and go home. And nobody tells Daddy she had better aim than me!\n<<>>\n";

/* ---- tuning ---- */
#define WAIT_T     1.8f     /* the gunship's wreck burns out, the music fades */
#define APPEAR_T   1.8f     /* she forms out of the motes */
#define READY_T    0.9f     /* after the scene: a beat before she moves */
#define DEATH_T    1.1f     /* her death animation, then she dissolves */
#define DISSOLVE_T 1.6f
#define GAP_MIN    100.0f   /* closer than this she backs off (a leap over the hero when cornered): she duels at range */
#define SHOT_SPEED 250.0f   /* the hero's shots fly at 500 */
#define SHOT_R     3.0f     /* shot radius in this duel, both ways (5 elsewhere): a crouch ducks a level shot, as it
                               should - with 5 the crouch box's top still caught it by a pixel */

enum { M_MIRROR, M_ATTACK, M_SLIDE, M_LEAP, M_RETREAT };
enum { DG_NONE, DG_CROUCH, DG_JUMP };
enum { B_L = 1, B_R = 2, B_U = 4, B_D = 8, B_J = 16, B_S = 32, B_A = 64 };

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void voice(const char *name)
{
    char f[64]; snprintf(f, sizeof f, "voice/%s.wav", name);
    const char *p = asset_path(f);
    if (p) sfx_play_file(p);
}

static void avatar_load(void)
{
    static bool done;
    if (done) return;
    done = true;
    const char *path = asset_path("dark_april_avatar.png"); int w, h;
    uint32_t *px = path ? png_load_rgba(path, &w, &h) : NULL;
    if (px) { sprite_from_rgba(namehash("dialog_avatar_darkapril"), px, w, h, 1); free(px); }
    else fprintf(stderr, "assets/dark_april_avatar.png not found\n");
}

static void mote(DarkApril *d, float x, float y, float vx, float vy, float life)
{
    for (int i = 0; i < DARK_MOTES; i++) {
        DarkMote *m = &d->motes[i];
        if (m->life > 0) continue;
        m->x = x; m->y = y; m->vx = vx; m->vy = vy; m->life = m->max = life;
        return;
    }
}

static void motes_update(DarkApril *d, float dt)
{
    for (int i = 0; i < DARK_MOTES; i++) {
        DarkMote *m = &d->motes[i];
        if (m->life <= 0) continue;
        m->life -= dt; m->x += m->vx * dt; m->y += m->vy * dt;
    }
}

void dark_begin(DarkApril *d, float arena_x, int sw, int difficulty)
{
    memset(d, 0, sizeof *d);
    d->state = DA_WAIT; d->arena_x = arena_x; d->sw = sw; d->difficulty = difficulty;
    d->hp = d->hp_max = difficulty == 0 ? 14 : difficulty == 1 ? 20 : 26;
    if (SDL_getenv("SABER_DARKHP")) d->hp = d->hp_max = atoi(SDL_getenv("SABER_DARKHP"));   /* debug */
    avatar_load();
    hero_quiet(true);   /* April's body, but the player's grunts stay the player's */
    player_spawn(&d->p, CRHC_APRIL, arena_x + sw - 80.0f, 120.0f);
    hero_quiet(false);
    d->p.quiet = true; d->p.locked = true;
    for (int b = 0; b < BTN_COUNT; b++) d->in.state[b] = 1;
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "dark april: arena %.0f, hp %d\n", arena_x, d->hp);
}

bool dark_holds_arena(const DarkApril *d) { return d->state != DA_OFF; }

/* the AI's held buttons -> the original's four button states (0 held, 2 pressed, 1 up, 3 released) */
static void make_input(DarkApril *d)
{
    for (int b = 0; b < BTN_COUNT; b++) {
        bool was = btn_down(&d->in, b);
        d->in.state[b] = d->want[b] ? (was ? 0 : 2) : (was ? 3 : 1);
    }
}

static uint8_t player_bits(const Input *in)
{
    uint8_t v = 0;
    if (btn_down(in, BTN_LEFT)) v |= B_L;
    if (btn_down(in, BTN_RIGHT)) v |= B_R;
    if (btn_down(in, BTN_UP)) v |= B_U;
    if (btn_down(in, BTN_DOWN)) v |= B_D;
    if (btn_down(in, BTN_JUMP)) v |= B_J;
    if (btn_down(in, BTN_SHOOT)) v |= B_S;
    if (btn_down(in, BTN_AIM)) v |= B_A;
    return v;
}

static bool on_ground(const Character *c) { return (c->coll & COLL_DOWN) != 0; }

/* a hero shot that will reach her in the next ~0.2 s: DG_CROUCH if it flies above her crouch, else DG_JUMP */
static int threat(const DarkApril *d, const Bullets *pb)
{
    const Character *c = &d->p.ch; const Body *b = &c->body;
    const HurtBox *h = &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0];
    float top = b->y + h->oy - h->hh, bot = b->y + h->oy + h->hh;
    for (int i = 0; i < pb->n; i++) {
        const Bullet *bl = &pb->b[i];
        int dir = bl->dir & 7;
        float vx = (dir == 0 || dir == 1 || dir == 7) ? -1.0f : (dir == 3 || dir == 4 || dir == 5) ? 1.0f : 0.0f;
        if (vx == 0) continue;
        float ahead = (b->x - bl->x) * vx;
        if (ahead < -8.0f || ahead > 130.0f) continue;
        float t = ahead / (bl->speed > 1 ? bl->speed : 1);
        float y = bl->y + ((dir == 1 || dir == 3) ? -1.0f : (dir == 5 || dir == 7) ? 1.0f : 0.0f) * bl->speed * 0.7f * t;
        if (y < top - SHOT_R || y > bot + SHOT_R) continue;
        const HurtBox *hc = &c->hurt[c->facing ? 0x29 : 0x28];   /* her crouch */
        return y < b->y + hc->oy - hc->hh - SHOT_R ? DG_CROUCH : DG_JUMP;
    }
    return DG_NONE;
}

static void pick_mode(DarkApril *d, const Character *P, float adx, bool phase2)
{
    float r = frand();
    bool grounded = on_ground(P);
    if (grounded && adx > 130.0f && adx < 220.0f && r < (phase2 ? 0.25f : 0.15f)) d->mode = M_SLIDE;
    else if (r < (phase2 ? 0.55f : 0.6f)) d->mode = M_MIRROR;
    else d->mode = M_ATTACK;
    d->mode_t = d->mode == M_MIRROR ? 1.6f + frand() * 1.8f : d->mode == M_ATTACK ? 1.2f + frand() * 1.3f : 1.0f;
    d->pref = 130.0f + frand() * 70.0f;
    d->crouch_shoot = frand() < 0.35f;
}

static void think(DarkApril *d, const Player *pl, const Bullets *pb, float dt)
{
    Character *c = &d->p.ch; const Character *P = &pl->ch;
    bool *w = d->want;
    memset(w, 0, sizeof d->want);
    float dx = P->body.x - c->body.x, adx = fabsf(dx);
    bool right = dx > 0, phase2 = d->hp * 2 <= d->hp_max;
    int toward = right ? BTN_RIGHT : BTN_LEFT, away = right ? BTN_LEFT : BTN_RIGHT;
    float left_room = c->body.x - d->arena_x, right_room = d->arena_x + d->sw - c->body.x;
    bool player_down = P->state == CS_DEAD;

    d->mode_t -= dt; d->dodge_cd -= dt; d->counter_t -= dt;
    float back_room = right ? left_room : right_room;
    if (adx < GAP_MIN && on_ground(c) && c->state != CS_SLIDE && d->mode != M_LEAP && d->mode != M_RETREAT && !player_down) {
        d->mode = M_RETREAT; d->mode_t = 2.0f; d->pref = 140.0f + frand() * 50.0f; d->counter_t = 0; d->dodge_t = 0;
    }
    /* the hero firing level at her while standing: she reads it, ducks under it and answers low (to be jumped) */
    bool level_fire = (P->flags & CF_SHOOT) && on_ground(P) && P->state != CS_CROUCH && P->state != CS_SLIDE &&
                      (P->aim == (right ? AIM_L : AIM_R));
    if (level_fire && d->counter_t <= -0.4f && d->mode != M_SLIDE && d->mode != M_LEAP && d->mode != M_RETREAT) {
        float chance = (d->difficulty == 0 ? 0.35f : d->difficulty == 1 ? 0.55f : 0.7f) + (phase2 ? 0.15f : 0.0f);
        d->counter_t = frand() < chance ? 0.7f + frand() * 0.5f : 0.0f;
    }
    if (d->counter_t > 0 && on_ground(c) && c->state != CS_SLIDE) {
        if (c->facing != (right ? 1 : 0)) { w[BTN_AIM] = true; w[toward] = true; }   /* turn first */
        else { w[BTN_DOWN] = true; w[BTN_SHOOT] = !player_down; }
        return;
    }
    if (d->mode_t <= 0 && c->state != CS_SLIDE && on_ground(c) && d->mode != M_RETREAT) pick_mode(d, P, adx, phase2);
    if (d->player_idle > 1.1f && d->mode == M_MIRROR) { d->mode = M_ATTACK; d->mode_t = 1.4f; d->pref = 100.0f + frand() * 60.0f; }

    /* dodging comes first: one roll per shot she sees coming (the cooldown), a better eye in the second half */
    if (d->dodge_t > 0) {
        d->dodge_t -= dt;
        if (d->dodge == DG_CROUCH) { w[BTN_DOWN] = true; w[BTN_SHOOT] = !player_down && frand() < 0.5f && c->facing == (right ? 1 : 0); return; }
        if (d->dodge == DG_JUMP) { w[BTN_JUMP] = d->dodge_t > 0.25f; if (back_room > 60.0f) w[away] = true; return; }   /* hop back, not at the hero */
    }
    if (d->dodge_cd <= 0 && on_ground(c) && c->state != CS_SLIDE) {
        int th = threat(d, pb);
        if (th) {
            d->dodge_cd = 0.45f;
            float chance = (d->difficulty == 0 ? 0.35f : d->difficulty == 1 ? 0.55f : 0.7f) + (phase2 ? 0.12f : 0.0f);
            if (frand() < chance) { d->dodge = th; d->dodge_t = th == DG_CROUCH ? 0.45f : 0.3f; think(d, pl, pb, 0); return; }
        }
    }

    switch (d->mode) {
    case M_MIRROR: {   /* the player's own buttons a moment ago, left and right swapped */
        int delay = phase2 ? 9 : 16;
        uint8_t v = d->hist[(d->hist_i - delay + DARK_HIST * 4) % DARK_HIST];
        w[BTN_LEFT] = v & B_R; w[BTN_RIGHT] = v & B_L; w[BTN_UP] = v & B_U; w[BTN_DOWN] = v & B_D;
        w[BTN_JUMP] = v & B_J; w[BTN_SHOOT] = (v & B_S) && !player_down; w[BTN_AIM] = v & B_A;
        if (adx < GAP_MIN + 40.0f && !w[BTN_AIM]) w[toward] = false;   /* mirrored, but she won't walk into the hero */
        break; }
    case M_ATTACK:
        if (player_down) { w[BTN_AIM] = true; w[toward] = true; break; }
        if (adx > d->pref + 24.0f) {                        /* close in, firing on the run */
            w[toward] = true; w[BTN_SHOOT] = frand() < 0.6f;
        } else if (d->crouch_shoot && on_ground(P) && c->facing == (right ? 1 : 0)) {
            w[BTN_DOWN] = true; w[BTN_SHOOT] = true;      /* low shots: the hero has to jump them */
        } else {                                            /* planted, 8-way aim at the hero's middle */
            float dy = (P->body.y - 10.0f) - c->body.y;
            w[BTN_AIM] = true; w[BTN_SHOOT] = true;
            if (dy < -adx * 0.45f) { w[BTN_UP] = true; if (dy > -adx * 2.4f) w[toward] = true; }
            else w[toward] = true;
        }
        break;
    case M_SLIDE:      /* down + toward, then jump: April's slide, right into the hero */
        if (c->state == CS_SLIDE) { w[toward] = false; break; }
        if (d->mode_t < 0.9f && on_ground(c)) { d->mode = M_RETREAT; d->mode_t = 2.0f; d->pref = 150.0f; break; }   /* through the hero: then away */
        w[toward] = true; w[BTN_DOWN] = true; w[BTN_JUMP] = true;
        break;
    case M_LEAP:       /* cornered: a somersault over the hero, then away on the other side */
        if (on_ground(c) && d->mode_t < 0.8f) { d->mode = M_RETREAT; d->mode_t = 2.0f; d->pref = 150.0f; break; }
        w[toward] = true; w[BTN_JUMP] = on_ground(c);
        break;
    case M_RETREAT:    /* back off to her range, then turn and duel */
        if (adx >= d->pref || d->mode_t <= 0 || (back_room < 40.0f && adx >= GAP_MIN)) {
            d->mode = M_ATTACK; d->mode_t = 1.2f + frand(); w[BTN_AIM] = true; w[toward] = true; break;
        }
        if (back_room < 40.0f) { d->mode = M_LEAP; d->mode_t = 1.0f; w[toward] = true; w[BTN_JUMP] = on_ground(c); break; }
        w[away] = true;
        break;
    }
    /* the arena's walls: backed into one she jumps out toward the middle */
    bool into_left = w[BTN_LEFT] && left_room < 28.0f, into_right = w[BTN_RIGHT] && right_room < 28.0f;
    if ((into_left || into_right) && on_ground(c) && d->mode != M_MIRROR && d->mode != M_RETREAT) {
        w[BTN_LEFT] = into_right; w[BTN_RIGHT] = into_left; w[BTN_JUMP] = true; w[BTN_AIM] = false;
    }
}

static void fire(DarkApril *d, Bullets *eb, Effects *fx, int layer)
{
    Player *p = &d->p; Character *c = &p->ch;
    if (!p->want_fire || p->fire_cooldown > 0.0f) return;
    static const float AIM_ANGLE[8] = { 3.1415927f, 2.3561945f, 1.5707964f, 0.7853982f, 0, 5.4977871f, 4.712389f, 3.9269908f };
    float x = c->body.x + c->muzzle_x, y = c->body.y + c->muzzle_y;
    AnimDef diag = { 0, 0, 4, 0, 0.015f, 0 }, straight = { 0, 4, 8, 4, 0.015f, 0 };
    Effect *e = effects_spawn(fx, 0x8623249C, layer, ((c->aim & 0xf9) == 1) ? &diag : &straight, x, y, 8, 8, AIM_ANGLE[c->aim & 7]);
    if (e) { e->follow_x = &c->body.x; e->follow_y = &c->body.y; e->fx0 = c->body.x; e->fy0 = c->body.y; }
    bullets_spawn(eb, BK_PLAYER, layer, x, y, c->aim & 7, SHOT_SPEED);
    if (eb->n) eb->b[eb->n - 1].dark = true;
    bool phase2 = d->hp * 2 <= d->hp_max;
    p->fire_cooldown = (phase2 ? 0.3f : 0.42f) + (d->difficulty == 0 ? 0.12f : d->difficulty == 2 ? -0.06f : 0.0f);
    sfx_play(1, 0);
}

static const HurtBox *hurt(const Character *c) { return &c->hurt[c->anim < CHAR_MAX_ANIMS ? c->anim : 0]; }

static bool overlap(const Character *a, const Character *b)
{
    const HurtBox *ha = hurt(a), *hb = hurt(b);
    return fabsf(a->body.x + ha->ox - b->body.x - hb->ox) <= ha->hw + hb->hw && fabsf(a->body.y + ha->oy - b->body.y - hb->oy) <= ha->hh + hb->hh;
}

static void take_hit(DarkApril *d, int dmg)
{
    d->hp -= dmg; d->flash = 0.1f;
    if (d->voice_t <= 0) { static const char *const H[3] = { "dark_hurt1", "dark_hurt2", "dark_hurt3" }; voice(H[rand() % 3]); d->voice_t = 0.9f; }
    if (d->mode == M_MIRROR && frand() < 0.3f) { d->mode = M_ATTACK; d->mode_t = 0.9f; d->pref = 120.0f; }   /* stung: she answers */
}

/* ours: a hero's power attack (power.c) catches her mid-fight: frac of her hit points */
void dark_power_hit(DarkApril *d, float frac)
{
    if (d->state != DA_FIGHT) return;
    take_hit(d, (int)ceilf(d->hp_max * frac));
    d->flash = 0.3f;
}

static void body_step(DarkApril *d, const Level *L, const PhysicsWorld *W, float dt)
{
    Character *c = &d->p.ch;
    PhysicsWorld aw = *W; aw.world_min_x = d->arena_x; aw.world_max_x = d->arena_x + d->sw;
    player_resolve(c, dt);
    physics_step(&aw, L, &c->body, dt);
    float lo = d->arena_x + 12.0f, hi = d->arena_x + d->sw - 12.0f;
    if (c->body.x < lo) c->body.x = lo;
    if (c->body.x > hi) c->body.x = hi;
    character_animate(c, dt);
}

static void face(Character *c, float x) { c->facing = x > c->body.x ? 1 : 0; c->aim = c->facing ? AIM_R : AIM_L; }

void dark_animate(DarkApril *d, float dt)
{
    if (d->state == DA_OFF || d->state == DA_DONE) return;
    character_animate(&d->p.ch, dt);
    motes_update(d, dt);
}

void dark_update(DarkApril *d, Player *pl, const Input *pin, const Level *L, const PhysicsWorld *W, Bullets *pb,
                 Bullets *eb, Effects *fx, int layer, float dt, bool live)
{
    if (d->state == DA_OFF || d->state == DA_DONE) return;
    Character *c = &d->p.ch;
    d->t += dt; d->flash -= dt; d->iframes -= dt; d->voice_t -= dt;
    motes_update(d, dt);
    int prev = d->state;
    switch (d->state) {
    case DA_WAIT:
        music_set_volume(1.0f - d->t / WAIT_T);
        if (!d->settled) {   /* she forms on the floor on the far side from the hero */
            float x = pl->ch.body.x < d->arena_x + d->sw * 0.5f ? d->arena_x + d->sw - 84.0f : d->arena_x + 84.0f;
            c->body.x = x; c->body.y = 120.0f;
            PhysicsWorld aw = *W; aw.world_min_x = d->arena_x; aw.world_max_x = d->arena_x + d->sw;
            for (int i = 0; i < 240 && !(c->body.coll & COLL_DOWN); i++) { character_sync_ground(c); player_resolve(c, 1 / 60.0f); physics_step(&aw, L, &c->body, 1 / 60.0f); }
            character_sync_ground(c); face(c, pl->ch.body.x); player_resolve(c, 0); character_animate(c, 0);
            d->settled = true;
        }
        if (d->t >= WAIT_T && live && pl->ch.state != CS_DEAD) { music_stop(); music_set_volume(1); d->state = DA_CALL; d->scene_call = true; }
        break;
    case DA_CALL:        /* the game shows the radio call; once it is closed she forms */
        if (live && !d->scene_call) d->state = DA_APPEAR;
        break;
    case DA_APPEAR: {
        float k = d->t / APPEAR_T; d->alpha = k < 1 ? k * k : 1;
        const HurtBox *h = hurt(c);
        for (int i = 0; i < 2 && k < 0.85f; i++) {   /* motes drawn in toward her from all around */
            float a = frand() * 6.2831853f, r = 40.0f + frand() * 50.0f;
            float tx = c->body.x + h->ox + (frand() - 0.5f) * 20.0f, ty = c->body.y + h->oy + (frand() - 0.5f) * 44.0f;
            mote(d, tx + cosf(a) * r, ty + sinf(a) * r, -cosf(a) * r / 0.6f, -sinf(a) * r / 0.6f, 0.6f);
        }
        face(c, pl->ch.body.x); character_sync_ground(c); player_resolve(c, dt); character_animate(c, dt);
        if (d->t > APPEAR_T * 0.5f && pl->ch.state == CS_IDLE) face(&pl->ch, c->body.x);   /* the hero turns to look */
        if (d->t >= APPEAR_T && live) { d->alpha = 1; d->state = DA_TALK; d->scene_meet = true; }
        break; }
    case DA_TALK:
        if (live && !d->scene_meet) { d->state = DA_READY; pl->hp = pl->max_hp; }   /* Ramrod tops the hero up for round two */
        break;
    case DA_READY:
        face(c, pl->ch.body.x); character_sync_ground(c); player_resolve(c, dt); character_animate(c, dt);
        if (d->t >= READY_T) { d->state = DA_FIGHT; d->p.locked = false; music_play(8, true); d->mode = M_MIRROR; d->mode_t = 1.2f; }
        break;
    case DA_FIGHT: {
        uint8_t bits = player_bits(pin);
        d->hist[d->hist_i] = bits; d->hist_i = (d->hist_i + 1) % DARK_HIST;
        d->player_idle = bits ? 0 : d->player_idle + dt;
        character_sync_ground(c);
        think(d, pl, pb, dt);
        make_input(d);
        player_control(&d->p, &d->in, dt);
        fire(d, eb, fx, layer);
        /* the hero's shots */
        const HurtBox *h = hurt(c);
        for (int i = 0; i < pb->n; i++) {
            Bullet *bl = &pb->b[i];
            if (fabsf(c->body.x + h->ox - bl->x) > SHOT_R + h->hw || fabsf(c->body.y + h->oy - bl->y) > SHOT_R + h->hh) continue;
            pb->b[i] = pb->b[--pb->n];
            take_hit(d, 1);
            break;
        }
        /* bodies: a slide goes through the other one and hurts it; otherwise touching her hurts the hero */
        if (pl->ch.state != CS_DEAD && overlap(c, &pl->ch)) {
            if (pl->ch.state == CS_SLIDE && c->state != CS_SLIDE) { if (d->iframes <= 0) { take_hit(d, 1); d->iframes = 0.8f; } }
            else if (!(pl->ch.flags & CF_HIT) && !(pl->ch.state == CS_SLIDE && c->state == CS_SLIDE)) player_damage(pl, c->facing ? 4 : 0, 1);
        }
        body_step(d, L, W, dt);
        player_frame_end(&d->p, dt);
        /* her afterimages while she moves fast (always in the second half) */
        bool fast = fabsf(c->body.vx) > 150.0f || !on_ground(c);
        if ((d->ghost_t -= dt) <= 0 && (fast || d->hp * 2 <= d->hp_max)) {
            d->ghost[d->ghost_i] = *c; d->ghost_i = (d->ghost_i + 1) % DARK_GHOSTS; if (d->nghost < DARK_GHOSTS) d->nghost++;
            d->ghost_t = 0.05f;
        } else if (!fast && d->hp * 2 > d->hp_max && d->ghost_t < -0.2f) d->nghost = 0;
        if (d->hp <= 0) {
            d->state = DA_DYING; d->p.locked = true; c->state = CS_DEAD; d->nghost = 0;
            for (int i = 0; i < eb->n; ) { if (eb->b[i].dark) eb->b[i] = eb->b[--eb->n]; else i++; }   /* her shots in flight fade with her */
            voice("dark_death1");
        }
        break; }
    case DA_DYING: {
        music_set_volume(1.0f - d->t / (DEATH_T + DISSOLVE_T));
        character_sync_ground(c); c->state = CS_DEAD; body_step(d, L, W, dt);
        float k = (d->t - DEATH_T) / DISSOLVE_T;
        if (k > 0) {
            d->alpha = k < 1 ? 1 - k : 0;
            const HurtBox *h = hurt(c);
            for (int i = 0; i < 3 && k < 1; i++)   /* she comes apart in rising motes */
                mote(d, c->body.x + h->ox + (frand() - 0.5f) * 2 * h->hw, c->body.y + h->oy + (frand() - 0.5f) * 2 * h->hh,
                     (frand() - 0.5f) * 30.0f, -30.0f - frand() * 50.0f, 0.5f + frand() * 0.7f);
        }
        if (k >= 1.3f) { d->state = DA_GONE; music_stop(); music_set_volume(1); }
        break; }
    case DA_GONE:
        if (d->t > 0.8f && live && pl->ch.state != CS_DEAD) { d->state = DA_DONE; d->scene_outro = true; }
        break;
    }
    if (d->state != prev) {
        d->t = 0;
        if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "dark april: state %d -> %d (hp %d)\n", prev, d->state, d->hp);
    }
    if (SDL_getenv("SABER_TRACE") && d->state == DA_FIGHT)
        fprintf(stderr, "dark: mode=%d st=%d x=%.0f y=%.0f hp=%d hero=%d/%d in=%d%d%d%d%d%d%d\n", d->mode, c->state, c->body.x, c->body.y, d->hp, pl->hp, pl->lives,
                d->want[0], d->want[1], d->want[2], d->want[3], d->want[4], d->want[5], d->want[6]);
}

/* her body with the sheet texture's colour, alpha and blend mode set for this draw (April's sheet is shared with the
 * hero when that is April: everything is put back) */
static void draw_as(const Character *c, float cx, float cy, uint8_t r, uint8_t g, uint8_t b, uint8_t a, SDL_BlendMode m)
{
    if (!c->cb || !c->cb->tex || !a) return;
    SDL_Texture *t = c->cb->tex;
    SDL_SetTextureColorMod(t, r, g, b); SDL_SetTextureAlphaMod(t, a); SDL_SetTextureBlendMode(t, m);
    character_draw(c, cx, cy);
    SDL_SetTextureColorMod(t, 255, 255, 255); SDL_SetTextureAlphaMod(t, 255); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
}

void dark_draw(const DarkApril *d, float cam_x, float cam_y)
{
    if (d->state == DA_OFF) return;
    const Character *c = &d->p.ch;
    SDL_Renderer *ren = c->cb && c->cb->tex ? SDL_GetRendererFromTexture(c->cb->tex) : NULL;
    if (d->state != DA_DONE && d->state != DA_GONE && d->state != DA_WAIT && d->state != DA_CALL) {
        float a = d->alpha;
        for (int i = 0; i < d->nghost; i++) {   /* oldest first, faintest */
            int k = (d->ghost_i + i) % DARK_GHOSTS;
            draw_as(&d->ghost[k], cam_x, cam_y, 110, 30, 170, (uint8_t)(a * (40 + 30 * i)), SDL_BLENDMODE_ADD);
        }
        /* a violet glow a pixel out all round, then the body as a dark shadow of April, a white flash on a hit */
        uint8_t ga = (uint8_t)(a * (190 + 50 * sinf(d->t * 6.0f)));
        draw_as(c, cam_x - 1, cam_y, 150, 40, 230, ga, SDL_BLENDMODE_ADD);
        draw_as(c, cam_x + 1, cam_y, 150, 40, 230, ga, SDL_BLENDMODE_ADD);
        draw_as(c, cam_x, cam_y - 1, 150, 40, 230, ga, SDL_BLENDMODE_ADD);
        draw_as(c, cam_x, cam_y + 1, 150, 40, 230, ga, SDL_BLENDMODE_ADD);
        draw_as(c, cam_x, cam_y, 84, 46, 118, (uint8_t)(a * 255), SDL_BLENDMODE_BLEND);
        if (d->flash > 0) draw_as(c, cam_x, cam_y, 255, 255, 255, (uint8_t)(a * 200), SDL_BLENDMODE_ADD);
    }
    if (!ren) return;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
    for (int i = 0; i < DARK_MOTES; i++) {
        const DarkMote *m = &d->motes[i];
        if (m->life <= 0) continue;
        float k = m->life / m->max;
        SDL_SetRenderDrawColor(ren, (uint8_t)(170 * k + 40), (uint8_t)(60 * k), (uint8_t)(255 * k), 255);
        SDL_FRect q = { floorf(m->x - cam_x), floorf(m->y - cam_y), k > 0.5f ? 2.0f : 1.0f, k > 0.5f ? 2.0f : 1.0f };
        SDL_RenderFillRect(ren, &q);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
}

void dark_draw_hud(const DarkApril *d, SDL_Renderer *ren, int sw)
{
    if (d->state != DA_READY && d->state != DA_FIGHT && d->state != DA_DYING) return;
    Font *f = font_get(0x12072E60);
    float bw = 96, x = sw - bw - 10, y = 20;
    if (f) font_draw(f, "DARK APRIL", (int)x, 8, 225, 200, 250);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 10, 8, 26, 200);
    SDL_FRect bg = { x - 1, y - 1, bw + 2, 7 };
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 170, 60, 240, 255);
    float k = d->hp > 0 ? (float)d->hp / (float)d->hp_max : 0;
    SDL_FRect fg = { x, y, bw * k, 5 };
    SDL_RenderFillRect(ren, &fg);
}
