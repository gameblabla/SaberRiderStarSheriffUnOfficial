#include "space.h"
#include "assets.h"
#include "font.h"
#include "audio.h"
#include "dialog.h"
#include "heroes.h"
#include "gfx.h"
#include "namehash.h"
#include "power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PI 3.1415927f

/* ---- the field: screen space; the world scrolls past from right to left ---- */
#define DRIFT 45.0f           /* px/s: a mine at rest in space, as the view flies past it */
#define PL_SPEED 128.0f
#define PL_SLOW 64.0f         /* AIM held: fine steering */
#define PL_R 3.0f             /* the hit core round the cockpit */
#define BOLT_V 390.0f
#define MINE_R 15.0f
#define TOP 12.0f
#define PRE_BOSS 106.0f       /* timeline seconds before the cruiser shows up */
#define MAX_FOE 96
#define MAX_SHOT 220
#define MAX_FX 320
#define MAX_CAP 8
#define MAX_GROUP 96
#define N_STAR 110
#define NEB_W 640

/* ---- atlas (../space/build.py) ---- */
enum { A_PLAYER, A_FIGHTER, A_DRONE, A_GUNSHIP, A_MINE, A_MLASER, A_TIP, A_BOLT, A_SPARK, A_ORB, A_TORPEDO, A_CAP,
       A_STAR, A_EXPL, A_SMOKE, A_FLASH, A_MFLASH, A_DEBRIS, A_PORT, A_COUNT };
static const char *const ANAMES[A_COUNT] = { "player", "fighter", "drone", "gunship", "mine", "mlaser", "tip", "bolt",
    "spark", "orb", "torpedo", "cap", "star", "expl", "smoke", "flash", "mflash", "debris", "port" };
#define MAX_FRAMES 8
typedef struct { int x, y, w, h, ax, ay; } Frame;
typedef struct { Frame f[MAX_FRAMES]; int n; } Anim;

/* ---- enemies ---- */
enum { F_OFF, F_MINE, F_FIGHTER, F_DRONE, F_GUNSHIP };
enum { P_LINE, P_SINE, P_SWOOP, P_REAR, P_DRIFT, P_BOB, P_SEEK, P_HOLD, P_SNAKE, P_LAUNCH };
enum { CAP_NONE, CAP_P, CAP_S, CAP_B };
typedef struct {
    int kind, pat, group; bool laser;
    float x, y, vx, vy, t, wait, hp, flash, fire_t, y0, amp, charge, cool, turn;
} Foe;
enum { S_BOLT, S_TORPEDO, S_ORB, S_MLASER };
typedef struct { bool on; int kind; float x, y, vx, vy, life; } Shot;
enum { FX_EXPL, FX_SMOKE, FX_FLASH, FX_SPARK, FX_DEBRIS, FX_CHUNK, FX_PULL };
typedef struct { bool on; int kind, frame; float x, y, vx, vy, t, dur, scale, rot, vr; SDL_FRect src; } Fx;
typedef struct { bool on; int kind; float x, y, t; } Cap;
typedef struct { int alive, drop; bool failed; } Group;

/* ---- the timeline ---- */
enum { E_MINE, E_LMINE, E_SMINE, E_FIGHT, E_GUN, E_MSG, E_RADIO, E_FAR };
typedef struct { float t; int ev, n; float y, gap; int pat, drop; } Ev;
/* mines: n of them at y, y+gap, ... (pat 1 = bobbing); fighters: n in a row, gap seconds apart; gunships: n at y,
 * y+gap. Minefield (6..40), the squadrons (41..82), the cruiser's escort (83..104). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const Ev TL[] = {
    { 2.5f, E_RADIO, 0 },
    { 5.0f, E_MSG, 0 },
    { 6.0f, E_MINE, 1, 120, 0, 0 },
    { 6.5f, E_RADIO, 1 },
    { 9.0f, E_MINE, 2, 60, 120, 0 },
    { 12.0f, E_MINE, 2, 40, 80, 1 },
    { 13.5f, E_FIGHT, 5, 196, 0.35f, P_LINE, CAP_P },
    { 15.5f, E_MINE, 2, 125, 80, 1 },
    { 18.0f, E_RADIO, 3 },
    { 18.0f, E_LMINE, 1, 90, 0, 0 },
    { 20.0f, E_MINE, 1, 190, 0, 1 },
    { 21.5f, E_LMINE, 1, 40, 0, 0 },
    { 22.5f, E_FIGHT, 5, 80, 0.3f, P_SINE, 0 },
    { 24.5f, E_MINE, 3, 50, 70, 0 },
    { 26.5f, E_LMINE, 1, 160, 0, 1 },
    { 28.0f, E_MINE, 2, 30, 180, 0 },
    { 29.0f, E_LMINE, 1, 120, 0, 0 },
    { 30.0f, E_RADIO, 7 },
    { 31.0f, E_FIGHT, 6, 170, 0.3f, P_SINE, CAP_S },
    { 32.5f, E_MINE, 2, 70, 90, 1 },
    { 34.5f, E_LMINE, 2, 40, 150, 0 },
    { 35.5f, E_FIGHT, 4, 115, 0.3f, P_LINE, 0 },
    { 37.0f, E_MINE, 3, 60, 60, 1 },
    { 39.0f, E_SMINE, 2, 50, 140, 0 },
    { 41.0f, E_MSG, 1 },
    { 41.0f, E_RADIO, 4 },
    { 42.0f, E_FIGHT, 6, 60, 0.28f, P_SINE, 0 },
    { 44.5f, E_FIGHT, 6, 180, 0.28f, P_SINE, CAP_P },
    { 46.0f, E_FAR },
    { 47.5f, E_GUN, 1, 120, 0, 0, CAP_B },
    { 51.0f, E_FIGHT, 4, 40, 0.25f, P_SWOOP, 0 },
    { 52.5f, E_FIGHT, 4, 200, 0.25f, P_SWOOP, 0 },
    { 55.0f, E_MINE, 2, 50, 140, 1 },
    { 56.5f, E_RADIO, 5 },
    { 57.0f, E_FIGHT, 5, 90, 0.3f, P_REAR, CAP_S },
    { 60.0f, E_GUN, 2, 60, 120, 0, CAP_P },
    { 63.5f, E_FIGHT, 5, 125, 0.3f, P_LINE, 0 },
    { 66.0f, E_FIGHT, 6, 50, 0.25f, P_SINE, 0 },
    { 66.0f, E_FIGHT, 6, 190, 0.25f, P_SINE, 0 },
    { 69.0f, E_LMINE, 2, 70, 100, 1 },
    { 70.0f, E_FIGHT, 4, 40, 0.25f, P_SWOOP, 0 },
    { 72.0f, E_GUN, 1, 170, 0, 0, CAP_B },
    { 73.0f, E_FIGHT, 4, 200, 0.25f, P_SWOOP, 0 },
    { 75.5f, E_FIGHT, 5, 150, 0.3f, P_REAR, 0 },
    { 78.0f, E_FIGHT, 8, 120, 0.2f, P_SINE, CAP_P },
    { 80.0f, E_GUN, 2, 50, 140, 0, 0 },
    { 83.5f, E_RADIO, 6 },
    { 84.0f, E_MSG, 2 },
    { 85.0f, E_LMINE, 3, 40, 80, 0 },
    { 86.5f, E_FIGHT, 6, 90, 0.25f, P_SINE, 0 },
    { 88.5f, E_SMINE, 2, 60, 120, 0 },
    { 89.5f, E_GUN, 1, 120, 0, 0, CAP_S },
    { 91.5f, E_FIGHT, 5, 30, 0.25f, P_SWOOP, 0 },
    { 92.5f, E_FIGHT, 5, 210, 0.25f, P_SWOOP, 0 },
    { 94.0f, E_MINE, 3, 40, 80, 1 },
    { 95.5f, E_FIGHT, 5, 60, 0.3f, P_REAR, 0 },
    { 97.0f, E_LMINE, 2, 90, 90, 0 },
    { 98.0f, E_GUN, 2, 60, 120, 0, CAP_P },
    { 100.0f, E_FIGHT, 8, 120, 0.18f, P_LINE, 0 },
    { 102.5f, E_SMINE, 3, 50, 70, 0 },
};
#pragma GCC diagnostic pop
#define N_TL ((int)(sizeof TL / sizeof *TL))
static const char *const MSGS[][2] = {
    { "OUTRIDER MINEFIELD", "THE MINES ARE ARMOURED - AVOID THEM" },
    { "OUTRIDER SQUADRONS", NULL },
    { "THE CRUISER'S ESCORT", NULL },
};

/* ---- the story. Ramrod is the whole crew's (as in phase 1). The Outriders fall back to the Phantom Zone when
 * they're beaten ("vaporized"), Nemesis leads them; the cruiser is running for a dimension jump. ---- */
typedef struct { const char *avatar, *text; } Radio;
static const Radio RADIO[] = {
    { "dialog_avatar_april2", "Cruiser mode, all green! The battle cruiser is dead ahead - and running for its jump point." },
    { "dialog_avatar_saber2", "It left a minefield in its wake. Those mines shrug off our guns - fly around them!" },
    { "dialog_avatar_colt2", "A power boost for the guns! Now we're cookin'." },
    { "dialog_avatar_april2", "Some of those mines carry lasers - they fire straight ahead. Don't sit in front of them!" },
    { "dialog_avatar_april2", "Outrider squadrons inbound - dozens of them!" },
    { "dialog_avatar_colt2", "Bogeys on our tail! They're coming up from behind!" },
    { "dialog_avatar_saber2", "The cruiser's escort. We're right on top of it - stay sharp!" },
    { "dialog_avatar_fireball1", "Torpedoes loaded. Jump button when it gets crowded - they even crack mines." },
    { "dialog_avatar_april2", "Its hangar's open - fighters launching! And it's dropping mines!" },
    { "dialog_avatar_fireball1", "It's pouring everything into that nose cannon - the beam follows us now. Keep moving!" },
    { "dialog_avatar_april2", "Hull integrity critical! One more hit and we're done!" },
    { "dialog_avatar_colt2", "Shields patched up. Thanks, darlin'." },
    { "dialog_avatar_saber2", "Its gun ports are down. Now hit that nose!" },
};
enum { R_POWER = 2, R_BOSS2 = 8, R_BOSS3 = 9, R_CRIT = 10, R_SHIELD = 11, R_PORTS = 12 };
static const char *const SCRIPT_BOSS =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nStar Sheriffs, this far out? You'll never reach the jump point alive. All gun ports - open fire!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nKnock out those gun ports, then hit the nose. Everything we've got!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nRamrod's never let us down. Let's ride!\n<<>>\n";
static const char *const SCRIPT_OUTRO =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nImpossible... Nemesis will... hear of... the Phantom Zone...!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nYee-haw! One battle cruiser, vaporized - with a bow on top!\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nNo jump signature. Nothing got through - Nemesis won't be sending a fleet after us.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThen the frontier is safe. Fine work, everyone. Ramrod... take us home.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nCourse set for Yuma. Next stop: a hot meal and a long, long nap.\n<<>>\n";

/* ---- the cruiser ---- */
#define MAX_PORT 8
typedef struct { float x, y, hp, fire_t, smoke_t; bool lit, dead; } Port;
enum { L_IDLE, L_CHARGE, L_FIRE, L_FADE };
typedef struct {
    bool on; float x, y, t, hp, hp_max, flash, bob_t;
    int stage;                    /* 0, 1 (under 2/3), 2 (under 1/3) */
    int laser; float laser_t, laser_cd, beam_len;
    float swarm_cd, mine_cd, burn_t;
    int swarm_left, swarm_n, swarm_group; float swarm_t;
    int swarms;
    Port port[MAX_PORT]; int nports;
    float ex, ey, bayx, bayy, rackx, racky;   /* in boss.png pixels */
    float die_t, expl_t, tilt; bool broken, ports_said;
} Boss;

enum { PH_INSTR, PH_PLAY, PH_WARNING, PH_BOSS_IN, PH_BOSS_TALK, PH_BOSS, PH_BOSS_DIE, PH_OUTRO, PH_CLEARED, PH_GAMEOVER };
#define INSTR_OPEN 0.5f
#define INSTR_MIN 2.4f
#define WARN_DUR 3.6f
#define BOSS_IN_DUR 5.0f

struct Space {
    SDL_Renderer *ren; int sw, sh; bool ok;
    SDL_Texture *atlas, *boss_tex, *neb, *planet, *far; Anim anim[A_COUNT];
    uint32_t *boss_px; int boss_w, boss_h, planet_w, planet_h, far_w, far_h;
    int difficulty, lives, result;
    /* Ramrod */
    float px, py, fire_cd, inv_t, dead_t, hurt_t, enter_t; int hp, hp_max, power, bombs; bool dead, fire_hold, bomb_hold;
    bool crit_said;
    /* world */
    Foe foe[MAX_FOE]; Shot shot[MAX_SHOT]; Fx fx[MAX_FX]; Cap cap[MAX_CAP]; Group group[MAX_GROUP]; int ngroup;
    Boss boss;
    float star_x[N_STAR], star_y[N_STAR]; int star_l[N_STAR];
    float neb_x, planet_x, far_x, far_y; bool far_on;
    Ev tl[N_TL]; int tl_next; float tl_t;
    /* flow */
    int phase; float phase_t, total_t;
    Dialog dlg; bool paused;
    char msg[48], msg2[48]; float msg_t;
    int radio_q[6], radio_n; float radio_t; int radio_now; bool power_said, shield_said;
    float white, red, black, shake, clink_t;
    unsigned rng; int music_now;
    bool god, bot;
    Power pw;                     /* the hero's power attack: the drawn cut-in, then a screen bomb (power.c) */
};

/* ---------------------------------------------------------------- helpers */
static float frand(Space *s) { s->rng = s->rng * 1664525u + 1013904223u; return (s->rng >> 8) / 16777216.0f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float approach(float v, float t, float d) { return v < t ? fminf(v + d, t) : fmaxf(v - d, t); }
static void set_msg(Space *s, const char *a, const char *b, float t) { snprintf(s->msg, sizeof s->msg, "%s", a); snprintf(s->msg2, sizeof s->msg2, "%s", b ? b : ""); s->msg_t = t; }
static void play_music(Space *s, int track) { if (s->music_now != track) { music_play(track, true); s->music_now = track; music_set_volume(1.0f); } }
static void sfx_file(const char *name) { char buf[64]; snprintf(buf, sizeof buf, "space/%s", name); sfx_play_file(asset_path(buf)); }
static float diffk(const Space *s) { return s->difficulty == 0 ? 0.72f : s->difficulty == 1 ? 1.0f : 1.3f; }
static float hpmul(const Space *s) { return s->difficulty == 0 ? 0.8f : s->difficulty == 1 ? 1.0f : 1.25f; }
static bool trace(void) { return SDL_getenv("SABER_TRACE") != NULL; }
static void radio(Space *s, int i) { if (s->radio_n < 6) s->radio_q[s->radio_n++] = i; }

/* ---------------------------------------------------------------- loading */
static SDL_Texture *load_tex(Space *s, const char *name, int *w, int *h, uint32_t **keep)
{
    char buf[64]; snprintf(buf, sizeof buf, "space/%s", name);
    const char *p = asset_path(buf);
    if (!p) { fprintf(stderr, "assets/%s missing (run ../space/build.py)\n", buf); return NULL; }
    int ww, hh; uint32_t *px = png_load_rgba(p, &ww, &hh);
    if (!px) return NULL;
    SDL_Texture *t = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, ww, hh);
    SDL_UpdateTexture(t, NULL, px, ww * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    if (w) *w = ww;
    if (h) *h = hh;
    if (keep) *keep = px; else free(px);
    return t;
}

static bool load_assets(Space *s)
{
    s->atlas = load_tex(s, "atlas.png", NULL, NULL, NULL);
    s->boss_tex = load_tex(s, "boss.png", &s->boss_w, &s->boss_h, &s->boss_px);
    s->neb = load_tex(s, "nebula.png", NULL, NULL, NULL);
    s->planet = load_tex(s, "planet.png", &s->planet_w, &s->planet_h, NULL);
    s->far = load_tex(s, "far_ship.png", &s->far_w, &s->far_h, NULL);
    if (!s->atlas || !s->boss_tex || !s->neb || !s->planet || !s->far) return false;
    const char *p = asset_path("space/atlas.txt");
    FILE *f = p ? fopen(p, "r") : NULL;
    if (!f) return false;
    char name[64]; int fr, x, y, fw, fh, ax, ay;
    while (fscanf(f, "%63s %d %d %d %d %d %d %d", name, &fr, &x, &y, &fw, &fh, &ax, &ay) == 8)
        for (int i = 0; i < A_COUNT; i++) if (!strcmp(name, ANAMES[i]) && fr < MAX_FRAMES) {
            s->anim[i].f[fr] = (Frame){ x, y, fw, fh, ax, ay }; if (fr + 1 > s->anim[i].n) s->anim[i].n = fr + 1;
        }
    fclose(f);
    for (int i = 0; i < A_COUNT; i++) if (!s->anim[i].n) { fprintf(stderr, "space atlas: %s missing\n", ANAMES[i]); return false; }
    p = asset_path("space/boss.txt");
    f = p ? fopen(p, "r") : NULL;
    if (!f) return false;
    char line[128]; Boss *b = &s->boss;
    while (fgets(line, sizeof line, f)) {
        float a, c; int lit;
        if (sscanf(line, "emitter %f %f", &a, &c) == 2) { b->ex = a; b->ey = c; }
        else if (sscanf(line, "bay %f %f", &a, &c) == 2) { b->bayx = a; b->bayy = c; }
        else if (sscanf(line, "rack %f %f", &a, &c) == 2) { b->rackx = a; b->racky = c; }
        else if (sscanf(line, "port %f %f %d", &a, &c, &lit) == 3 && b->nports < MAX_PORT) b->port[b->nports++] = (Port){ a, c, 0, 0, 0, lit != 0, false };
    }
    fclose(f);
    return b->nports > 0;
}

/* ---------------------------------------------------------------- effects */
static Fx *fx_new(Space *s) { for (int i = 0; i < MAX_FX; i++) if (!s->fx[i].on) { Fx *e = &s->fx[i]; memset(e, 0, sizeof *e); e->on = true; e->scale = 1; return e; } return NULL; }
static void spawn_expl(Space *s, float x, float y, float scale)
{
    Fx *e = fx_new(s); if (!e) return;
    e->kind = FX_EXPL; e->x = x; e->y = y; e->scale = scale; e->dur = 0.5f; e->vx = -DRIFT * 0.3f;
}
static void spawn_fx(Space *s, int kind, float x, float y, float scale, float dur)
{
    Fx *e = fx_new(s); if (!e) return;
    e->kind = kind; e->x = x; e->y = y; e->scale = scale; e->dur = dur;
    if (kind == FX_SMOKE) { e->vx = -DRIFT * 0.6f; e->vy = -6; }
}
static void spawn_debris(Space *s, float x, float y, int n, float speed)
{
    for (int k = 0; k < n; k++) {
        Fx *e = fx_new(s); if (!e) return;
        float a = frand(s) * 2 * PI, v = speed * (0.4f + frand(s));
        e->kind = FX_DEBRIS; e->x = x; e->y = y; e->vx = cosf(a) * v - DRIFT * 0.5f; e->vy = sinf(a) * v; e->frame = k % 6;
        e->dur = 0.9f + frand(s) * 0.8f; e->vr = (frand(s) - 0.5f) * 720;
    }
}
static void blast(Space *s, float x, float y, float size)   /* a ship going up: fireballs, a flash, debris */
{
    spawn_expl(s, x, y, size);
    for (int k = 0; k < (int)(size * 3); k++) spawn_expl(s, x + (frand(s) - 0.5f) * 30 * size, y + (frand(s) - 0.5f) * 20 * size, size * (0.4f + frand(s) * 0.4f));
    spawn_fx(s, FX_FLASH, x, y, size * 1.5f, 0.12f);
    spawn_debris(s, x, y, 4 + (int)(size * 4), 60 + 60 * size);
}

/* ---------------------------------------------------------------- spawning */
static Foe *foe_new(Space *s) { for (int i = 0; i < MAX_FOE; i++) if (s->foe[i].kind == F_OFF) { Foe *f = &s->foe[i]; memset(f, 0, sizeof *f); f->group = -1; return f; } return NULL; }
static int group_new(Space *s, int drop) { if (s->ngroup >= MAX_GROUP) return -1; s->group[s->ngroup] = (Group){ 0, drop, false }; return s->ngroup++; }

static void spawn_mine(Space *s, float x, float y, int pat, bool laser)
{
    Foe *f = foe_new(s); if (!f) return;
    f->kind = F_MINE; f->pat = pat; f->x = x; f->y = y; f->y0 = y; f->vx = -DRIFT; f->laser = laser;
    f->amp = pat == P_BOB ? 18 : 0; f->t = frand(s) * 6; f->cool = 0.8f + frand(s);
}

static void spawn_fighters(Space *s, int kind, int n, float y, float gap, int pat, int drop)
{
    int g = group_new(s, drop);
    for (int i = 0; i < n; i++) {
        Foe *f = foe_new(s); if (!f) return;
        f->kind = kind; f->pat = pat; f->group = g; f->wait = i * gap; f->y = f->y0 = y;
        f->hp = (kind == F_DRONE ? 2 : 3) * hpmul(s);
        f->x = pat == P_REAR ? -24.0f : s->sw + 24.0f;
        if (pat == P_REAR) f->wait += 1.2f;   /* the warning chevrons first */
        f->amp = pat == P_SINE ? 34 : 0;
        f->turn = y < s->sh * 0.5f ? 1 : -1;
        f->fire_t = frand(s) < 0.45f ? 1.2f + frand(s) * 2.0f : 1e9f;   /* under half of them shoot */
        if (g >= 0) s->group[g].alive++;
    }
}

static void spawn_gunship(Space *s, float y, int g)
{
    Foe *f = foe_new(s); if (!f) return;
    f->kind = F_GUNSHIP; f->pat = P_HOLD; f->group = g; f->x = s->sw + 40.0f; f->y = f->y0 = y;
    f->hp = 22 * hpmul(s); f->fire_t = 1.2f; f->t = 0; f->vx = -80;
    if (g >= 0) s->group[g].alive++;
}

static void fire_orb(Space *s, float x, float y, float ang, float v)
{
    for (int i = 0; i < MAX_SHOT; i++) if (!s->shot[i].on) {
        s->shot[i] = (Shot){ true, S_ORB, x, y, cosf(ang) * v, sinf(ang) * v, 6.0f };
        return;
    }
}
static float aim_at(Space *s, float x, float y) { return atan2f(s->py - y, s->px - x); }
static float orb_v(Space *s) { return 105 * (0.85f + 0.15f * diffk(s)) * (s->difficulty == 2 ? 1.15f : 1.0f); }

static void run_event(Space *s, const Ev *e)
{
    switch (e->ev) {
    case E_MINE: case E_LMINE: case E_SMINE:
        for (int i = 0; i < e->n; i++)
            spawn_mine(s, s->sw + 30.0f, e->y + i * e->gap, e->ev == E_SMINE ? P_SEEK : e->pat ? P_BOB : P_DRIFT, e->ev == E_LMINE);
        break;
    case E_FIGHT: spawn_fighters(s, F_FIGHTER, e->n, e->y, e->gap, e->pat, e->drop); break;
    case E_GUN: { int g = group_new(s, e->drop); for (int i = 0; i < e->n; i++) spawn_gunship(s, e->y + i * e->gap, g); break; }
    case E_MSG: set_msg(s, MSGS[e->n][0], MSGS[e->n][1], 2.6f); break;
    case E_RADIO: radio(s, e->n); break;
    case E_FAR: s->far_on = true; s->far_x = (float)s->sw; s->far_y = 150; break;
    }
}

/* ---------------------------------------------------------------- setup */
static int cmp_ev(const void *a, const void *b) { float x = ((const Ev *)a)->t, y = ((const Ev *)b)->t; return x < y ? -1 : x > y; }

Space *space_create(SDL_Renderer *ren, int sw, int sh, int difficulty, int lives, int hero)
{
    Space *s = calloc(1, sizeof *s);
    power_reset(&s->pw, hero, true);
    s->ren = ren; s->sw = sw; s->sh = sh; s->rng = 0x7E57AB1Eu; s->music_now = -1; s->radio_now = -1;
    s->ok = load_assets(s);
    s->difficulty = difficulty; s->lives = lives;
    s->hp = s->hp_max = difficulty == 0 ? 5 : difficulty == 1 ? 4 : 3;
    s->power = 1; s->bombs = 3; s->px = -40; s->py = sh * 0.5f; s->enter_t = 1.0f;
    s->god = SDL_getenv("SABER_R7GOD") != NULL; s->bot = SDL_getenv("SABER_R7BOT") != NULL;
    dialog_set_hero(HERO_FIREBALL);   /* the scenes are written for the whole crew, as in phase 1 */
    for (int i = 0; i < N_STAR; i++) { s->star_x[i] = frand(s) * sw; s->star_y[i] = frand(s) * sh; s->star_l[i] = i % 3; }
    memcpy(s->tl, TL, sizeof TL); qsort(s->tl, N_TL, sizeof *s->tl, cmp_ev);
    s->planet_x = 0;
    s->phase = PH_INSTR;
    play_music(s, 12);
    if (SDL_getenv("SABER_R7T")) {   /* debug: start the timeline at t seconds (the events before it are skipped) */
        s->tl_t = (float)atof(SDL_getenv("SABER_R7T"));
        while (s->tl_next < N_TL && s->tl[s->tl_next].t < s->tl_t) s->tl_next++;
        s->planet_x = -s->tl_t * 8; s->phase = PH_PLAY; s->enter_t = 0; s->px = 60;
    }
    if (SDL_getenv("SABER_R7BOSS")) { s->tl_t = PRE_BOSS; s->tl_next = N_TL; s->phase = PH_PLAY; s->enter_t = 0; s->px = 60; s->planet_x = -1000; }
    return s;
}

void space_destroy(Space *s)
{
    if (!s) return;
    SDL_Texture *t[] = { s->atlas, s->boss_tex, s->neb, s->planet, s->far };
    for (int i = 0; i < 5; i++) if (t[i]) SDL_DestroyTexture(t[i]);
    power_close(&s->pw);
    free(s->boss_px); free(s);
}
int space_result(const Space *s) { return s->result; }
int space_lives(const Space *s) { return s->lives; }

/* ---------------------------------------------------------------- the cruiser's shape */
static bool boss_solid(const Space *s, float x, float y)
{
    const Boss *b = &s->boss;
    if (!b->on || b->broken) return false;
    int ix = (int)floorf(x - b->x), iy = (int)floorf(y - b->y);
    if (ix < 0 || iy < 0 || ix >= s->boss_w || iy >= s->boss_h) return false;
    return (s->boss_px[iy * s->boss_w + ix] >> 24) > 0;
}
static float boss_ex(const Space *s) { return s->boss.x + s->boss.ex; }
static float boss_ey(const Space *s) { return s->boss.y + s->boss.ey; }
static float beam_half(const Space *s) { return s->boss.stage >= 2 ? 11.0f : 5.5f; }

/* ---------------------------------------------------------------- Ramrod */
static bool alive(const Space *s) { return !s->dead && s->enter_t <= 0; }
static bool vulnerable(const Space *s) { return alive(s) && s->inv_t <= 0 && !s->god && (s->phase == PH_PLAY || s->phase == PH_WARNING || s->phase == PH_BOSS_IN || s->phase == PH_BOSS); }

static void player_die(Space *s)
{
    s->dead = true; s->dead_t = 0; s->hp = 0;
    blast(s, s->px, s->py, 1.4f);
    sfx_play(5, 0); sfx_play(6, 4); sfx_play(0x15, 8);
    s->shake = 0.6f; s->white = 0.5f;
    if (s->power > 1) s->power--;
    if (trace()) fprintf(stderr, "r7 died t=%.1f lives=%d\n", s->tl_t, s->lives);
}

static void player_hurt(Space *s, const char *what)
{
    if (!vulnerable(s)) return;
    if (trace()) fprintf(stderr, "r7 hurt %s hp=%d t=%.1f\n", what, s->hp, s->tl_t);
    s->hp--; s->inv_t = 1.5f; s->hurt_t = 0.5f; s->red = 0.6f; s->shake = fmaxf(s->shake, 0.3f);
    sfx_play(3, 0);
    spawn_fx(s, FX_FLASH, s->px, s->py, 1.6f, 0.12f);
    if (s->hp <= 0) player_die(s);
    else if (s->hp == 1 && !s->crit_said) { s->crit_said = true; radio(s, R_CRIT); }
}

static void foe_kill(Space *s, Foe *f, bool by_player);

static void detonate_torpedo(Space *s, float x, float y)
{
    s->white = fmaxf(s->white, 0.75f); s->shake = fmaxf(s->shake, 0.5f);
    sfx_play(6, 0);
    for (int k = 0; k < 10; k++) spawn_expl(s, x + (frand(s) - 0.5f) * 80, y + (frand(s) - 0.5f) * 60, 1.0f + frand(s));
    for (int i = 0; i < MAX_SHOT; i++) if (s->shot[i].on && s->shot[i].kind >= S_ORB) { spawn_fx(s, FX_SPARK, s->shot[i].x, s->shot[i].y, 1, 0.2f); s->shot[i].on = false; }
    for (int i = 0; i < MAX_FOE; i++) {
        Foe *f = &s->foe[i];
        if (f->kind == F_OFF || f->wait > 0 || f->x < -20 || f->x > s->sw + 20) continue;
        if (f->kind == F_MINE) foe_kill(s, f, true);
        else { f->hp -= 14; f->flash = 0.15f; if (f->hp <= 0) foe_kill(s, f, true); }
    }
    Boss *b = &s->boss;
    if (b->on && s->phase == PH_BOSS) {
        b->hp -= 28; b->flash = 0.2f;
        for (int k = 0; k < b->nports; k++) if (b->port[k].lit && !b->port[k].dead) b->port[k].hp -= 10;
    }
    if (alive(s)) s->inv_t = fmaxf(s->inv_t, 1.0f);
}

/* the hero's power attack (power.c) goes off once its cut-in ends: every enemy shot burns out, the mines and the
 * light craft on screen are torn apart, the rest and the cruiser take a heavy hit, and Ramrod gets a moment's
 * grace. A cut above a torpedo, but no finisher: 7 % of the hull. */
static void hero_bomb(Space *s)
{
    s->white = 1.0f; s->shake = fmaxf(s->shake, 0.8f);
    sfx_play(6, 0); sfx_play(5, 6);
    for (int k = 0; k < 18; k++) spawn_expl(s, frand(s) * s->sw, TOP + frand(s) * (s->sh - TOP), 1.0f + frand(s) * 1.2f);
    for (int i = 0; i < MAX_SHOT; i++) if (s->shot[i].on && s->shot[i].kind >= S_ORB) { spawn_fx(s, FX_SPARK, s->shot[i].x, s->shot[i].y, 1, 0.2f); s->shot[i].on = false; }
    for (int i = 0; i < MAX_FOE; i++) {
        Foe *f = &s->foe[i];
        if (f->kind == F_OFF || f->wait > 0 || f->x < -20 || f->x > s->sw + 20) continue;
        if (f->kind == F_MINE) foe_kill(s, f, true);
        else { f->hp -= 40; f->flash = 0.2f; if (f->hp <= 0) foe_kill(s, f, true); }
    }
    Boss *b = &s->boss;
    if (b->on && s->phase == PH_BOSS) {
        b->hp -= b->hp_max * 0.07f; b->flash = 0.3f;
        for (int k = 0; k < b->nports; k++) if (b->port[k].lit && !b->port[k].dead) b->port[k].hp -= 25;
    }
    if (alive(s)) s->inv_t = fmaxf(s->inv_t, 2.5f);
}

static void fire_bolts(Space *s)
{
    static const struct { float dx, dy, vy; int lvl; } B[] = {
        { 22, -3, 0, 1 }, { 22, 3, 0, 1 },
        { 12, -5, -75, 2 }, { 12, 5, 75, 2 },
        { 24, 0, 0, 3 }, { 8, -6, -170, 3 }, { 8, 6, 170, 3 },
    };
    for (int k = 0; k < (int)(sizeof B / sizeof *B); k++) {
        if (B[k].lvl > s->power) continue;
        if (s->power == 3 && B[k].lvl == 2) continue;   /* level 3 trades the narrow pair for a wide one + a centre bolt */
        for (int i = 0; i < MAX_SHOT; i++) if (!s->shot[i].on) {
            float vx = BOLT_V, vy = B[k].vy, n = sqrtf(vx * vx + vy * vy);
            s->shot[i] = (Shot){ true, S_BOLT, s->px + B[k].dx, s->py + B[k].dy, vx / n * BOLT_V, vy / n * BOLT_V, 1.5f };
            break;
        }
    }
    sfx_file("shot.wav");
}

static void player_control(Space *s, const Input *in, float dt)
{
    if (s->dead) return;
    if (s->enter_t > 0) {   /* flying in from the left edge */
        s->enter_t -= dt; s->px = approach(s->px, 64, 140 * dt);
        if (s->enter_t <= 0) { s->enter_t = 0; s->fire_hold = btn_down(in, BTN_SHOOT); s->bomb_hold = true; }
        return;
    }
    float sp = btn_down(in, BTN_AIM) ? PL_SLOW : PL_SPEED, mx = 0, my = 0;
    if (btn_down(in, BTN_LEFT)) mx -= 1;
    if (btn_down(in, BTN_RIGHT)) mx += 1;
    if (btn_down(in, BTN_UP)) my -= 1;
    if (btn_down(in, BTN_DOWN)) my += 1;
    if (mx && my) { mx *= 0.7071f; my *= 0.7071f; }
    s->px = clampf(s->px + mx * sp * dt, 14, s->sw - 20.0f); s->py = clampf(s->py + my * sp * dt, TOP, s->sh - 10.0f);
    for (int k = 0; k < 80 && boss_solid(s, s->px + 6, s->py); k++) s->px -= 1;   /* the hull shoves Ramrod back */
    if (boss_solid(s, s->px + 12, s->py) && s->phase == PH_BOSS) player_hurt(s, "hull");
    if (s->dlg.active) s->fire_hold = s->bomb_hold = true;
    if (!btn_down(in, BTN_SHOOT)) s->fire_hold = false;
    if (!btn_down(in, BTN_JUMP)) s->bomb_hold = false;
    s->fire_cd -= dt;
    if (btn_down(in, BTN_SHOOT) && !s->fire_hold && s->fire_cd <= 0) { fire_bolts(s); s->fire_cd = s->power >= 3 ? 0.095f : 0.11f; }
    bool fighting = s->phase == PH_PLAY || s->phase == PH_WARNING || s->phase == PH_BOSS_IN || s->phase == PH_BOSS;
    if (btn_pressed(in, BTN_POWER) && fighting && !s->dlg.active && power_can_start(&s->pw)) { power_start(&s->pw, s->ren); s->fire_hold = true; }
    if (btn_pressed(in, BTN_JUMP) && !s->bomb_hold && s->bombs > 0) {
        for (int i = 0; i < MAX_SHOT; i++) if (!s->shot[i].on) { s->shot[i] = (Shot){ true, S_TORPEDO, s->px + 20, s->py, 260, 0, 0.4f }; break; }
        s->bombs--; sfx_file("torpedo.wav");
    }
}

/* ---------------------------------------------------------------- enemies */
static void foe_kill(Space *s, Foe *f, bool by_player)
{
    float sz = f->kind == F_GUNSHIP ? 1.3f : f->kind == F_MINE ? 1.1f : 0.7f;
    blast(s, f->x, f->y, sz);
    sfx_play(f->kind == F_GUNSHIP ? 6 : 5, 0);
    if (f->group >= 0 && f->group < s->ngroup) {
        Group *g = &s->group[f->group];
        if (!by_player) g->failed = true;
        if (--g->alive == 0 && !g->failed && g->drop) {
            for (int i = 0; i < MAX_CAP; i++) if (!s->cap[i].on) { s->cap[i] = (Cap){ true, g->drop, f->x, f->y, 0 }; break; }
        }
    }
    f->kind = F_OFF;
}

static void foe_escape(Space *s, Foe *f)
{
    if (f->group >= 0 && f->group < s->ngroup) { s->group[f->group].failed = true; s->group[f->group].alive--; }
    f->kind = F_OFF;
}

static void foe_update(Space *s, Foe *f, float dt)
{
    if (f->wait > 0) { f->wait -= dt; return; }
    f->t += dt; if (f->flash > 0) f->flash -= dt;
    float k = diffk(s);
    switch (f->pat) {
    case P_DRIFT: f->vx = -DRIFT; f->vy = 0; break;
    case P_BOB: f->vx = -DRIFT; f->y = f->y0 + sinf(f->t * 1.3f) * f->amp; f->vy = 0; break;
    case P_SEEK: f->vx = -40; f->vy = approach(f->vy, clampf((s->py - f->y) * 0.8f, -28, 28), 40 * dt); break;
    case P_LAUNCH: f->vx = approach(f->vx, -DRIFT, 60 * dt); f->vy = approach(f->vy, 0, 30 * dt); break;
    case P_LINE: f->vx = -150; f->vy = 0; break;
    case P_SINE: f->vx = -115; f->y = f->y0 + sinf(f->t * 2.6f) * f->amp; f->vy = 0; break;
    case P_SWOOP:   /* straight in, then peel off up or down, turning back to the right */
        if (f->cool == 0 && f->x > s->sw * 0.42f) { f->vx = -175; f->vy = 0; }
        else { f->cool = 1; f->vx = approach(f->vx, 90, 200 * dt); f->vy = approach(f->vy, -f->turn * 110, 260 * dt); }
        break;
    case P_REAR: f->vx = 175; f->y = f->y0 + sinf(f->t * 3.0f) * 12; f->vy = 0; break;
    case P_SNAKE:   /* the cruiser's swarm: a column that bends toward Ramrod */
        f->vx = -150; f->y0 = approach(f->y0, s->py, 32 * dt); f->y = f->y0 + sinf(f->t * 3.4f) * 22; f->vy = 0; break;
    case P_HOLD: {  /* a gunship: in, hold station and fire, then drift off */
        float hold_x = s->sw - 64.0f;
        if (f->t < 8.0f) { f->vx = f->x > hold_x ? -80 : 0; f->y = f->y0 + sinf(f->t * 1.1f) * 10; f->vy = 0; }
        else f->vx = approach(f->vx, -60, 40 * dt);
        break; }
    }
    f->x += f->vx * dt; f->y += f->vy * dt;
    /* weapons */
    if (f->kind == F_MINE && f->laser) {
        f->cool -= dt;
        bool front = s->px < f->x - 30 && fabsf(s->py - f->y) < 34 && f->x < s->sw - 12 && alive(s);
        if (f->charge > 0) {
            f->charge += dt;
            if (f->charge >= 0.65f) {
                for (int i = 0; i < MAX_SHOT; i++) if (!s->shot[i].on) { s->shot[i] = (Shot){ true, S_MLASER, f->x - 22, f->y - 1, -250 * (0.85f + 0.15f * k), 0, 4.0f }; break; }
                f->charge = 0; f->cool = 2.4f / k; sfx_file("zap.wav");
            }
        } else if (front && f->cool <= 0) f->charge = 0.001f;
    } else if (f->kind == F_FIGHTER) {
        f->fire_t -= dt * k;
        if (f->fire_t <= 0 && f->x > 40 && f->x < s->sw - 16 && alive(s)) { fire_orb(s, f->x - 10, f->y, aim_at(s, f->x, f->y), orb_v(s)); f->fire_t = (2.8f + frand(s) * 2) ; sfx_play(7, 0); }
    } else if (f->kind == F_GUNSHIP) {
        f->fire_t -= dt * k;
        if (f->fire_t <= 0 && f->x < s->sw - 20 && f->x > 30 && alive(s)) {
            int n = s->difficulty == 2 ? 5 : 3; float a = aim_at(s, f->x - 28, f->y);
            for (int i = 0; i < n; i++) fire_orb(s, f->x - 28, f->y, a + (i - (n - 1) * 0.5f) * 0.2f, orb_v(s) * 0.95f);
            f->fire_t = 1.7f; sfx_play(7, 0);
        }
    }
    /* off the field */
    if (f->x < -60 || f->x > s->sw + 80 || f->y < -60 || f->y > s->sh + 60) {
        if (f->kind == F_MINE) f->kind = F_OFF; else foe_escape(s, f);
    }
}

typedef struct { float hw, hh, oy; } Box;
static Box foe_box(const Foe *f)
{
    switch (f->kind) {
    case F_GUNSHIP: return (Box){ 30, 8, 1 };
    case F_MINE: return (Box){ MINE_R + 3, MINE_R + 3, 0 };
    default: return (Box){ 17, 6, 0 };
    }
}
static bool in_box(const Foe *f, float x, float y, float pad)
{
    Box b = foe_box(f);
    if (f->kind == F_MINE) { float dx = x - f->x, dy = y - f->y; return dx * dx + dy * dy < (MINE_R + pad) * (MINE_R + pad); }
    return fabsf(x - f->x) < b.hw + pad && fabsf(y - (f->y + b.oy)) < b.hh + pad;
}

/* ---------------------------------------------------------------- the cruiser */
static void boss_start(Space *s)
{
    Boss *b = &s->boss;
    b->on = true; b->x = s->sw + 10.0f; b->y = 40; b->hp = b->hp_max = 1200 * hpmul(s);
    if (SDL_getenv("SABER_R7BOSSHP")) b->hp = b->hp_max = (float)atof(SDL_getenv("SABER_R7BOSSHP"));
    for (int k = 0; k < b->nports; k++) { b->port[k].hp = 20 * hpmul(s); b->port[k].fire_t = 1.5f + k * 0.45f; b->port[k].dead = false; }
    b->laser = L_IDLE; b->laser_cd = 4.0f; b->swarm_cd = 7.0f; b->mine_cd = 5.0f; b->stage = 0;
}

static void boss_launch_swarm(Space *s)
{
    Boss *b = &s->boss;
    int drop = (b->swarms % 2 == 1) ? (b->swarms % 4 == 1 ? CAP_S : CAP_B) : CAP_NONE;   /* every other swarm leaves a gift */
    if (b->stage == 0 && b->swarms == 0) drop = CAP_P;
    b->swarm_group = group_new(s, drop); b->swarm_left = b->swarm_n = 5 + b->stage; b->swarm_t = 0; b->swarms++;
    sfx_file("launch.wav");
}

static void boss_update(Space *s, float dt)
{
    Boss *b = &s->boss; float k = diffk(s);
    b->t += dt; if (b->flash > 0) b->flash -= dt;
    float x_final = s->sw - (float)s->boss_w;
    if (s->phase == PH_BOSS_IN) {
        float p = clampf(s->phase_t / BOSS_IN_DUR, 0, 1); p = 1 - (1 - p) * (1 - p) * (1 - p);
        b->x = s->sw + 10 + (x_final - s->sw - 10) * p;
    } else b->x = x_final;
    /* the bob: frozen while the nose cannon charges and fires; in the last stage the beam chases Ramrod */
    if (b->laser == L_IDLE || b->laser == L_FADE) { b->bob_t += dt; b->y = approach(b->y, 40 + sinf(b->bob_t * 0.7f) * 18, 30 * dt); }
    else if (b->laser == L_FIRE && b->stage >= 2) b->y = clampf(approach(b->y, s->py - b->ey, 26 * dt), -40, 150);
    if (s->phase != PH_BOSS) return;
    /* stages */
    float f = b->hp / b->hp_max;
    if (b->stage == 0 && f < 0.66f) {
        b->stage = 1; radio(s, R_BOSS2); b->mine_cd = 1.5f;
        for (int i = 0; i < b->nports; i++) if (!b->port[i].lit) { b->port[i].lit = true; b->port[i].fire_t = 1.0f + i * 0.3f; }   /* the strut's spare ports open */
        s->shake = 0.4f; sfx_play(0x13, 0);
    }
    if (b->stage == 1 && f < 0.33f) { b->stage = 2; radio(s, R_BOSS3); s->shake = 0.5f; sfx_play(0x13, 0); b->laser_cd = fminf(b->laser_cd, 2.0f); }
    /* gun ports */
    bool beam = b->laser == L_FIRE;
    int live = 0;
    for (int i = 0; i < b->nports; i++) {
        Port *p = &b->port[i];
        if (!p->lit || p->dead) { if (p->dead && (p->smoke_t -= dt) <= 0) { p->smoke_t = 0.35f + frand(s) * 0.3f; spawn_fx(s, FX_SMOKE, b->x + p->x, b->y + p->y, 0.6f, 1.0f); } continue; }
        live++;
        if (beam && s->difficulty < 2) continue;   /* they hold fire under the beam (not on hard) */
        p->fire_t -= dt * k;
        if (p->fire_t <= 0 && alive(s)) {
            float x = b->x + p->x - 4, y = b->y + p->y + 3, a = aim_at(s, x, y);
            bool strut = p->x > 170;
            int n = strut && b->stage >= 1 ? 3 : 1;
            for (int j = 0; j < n; j++) fire_orb(s, x, y, a + (j - (n - 1) * 0.5f) * 0.22f + (frand(s) - 0.5f) * 0.08f, orb_v(s));
            p->fire_t = (b->stage == 0 ? 3.2f : b->stage == 1 ? 2.7f : 2.2f) + frand(s) * 0.8f;
            spawn_fx(s, FX_FLASH, x, y, 0.5f, 0.08f);
        }
    }
    if (live == 0 && !b->ports_said && b->stage >= 1) { b->ports_said = true; radio(s, R_PORTS); }
    /* the nose cannon */
    b->laser_t += dt;
    switch (b->laser) {
    case L_IDLE:
        b->laser_cd -= dt;
        if (b->laser_cd <= 0 && alive(s)) { b->laser = L_CHARGE; b->laser_t = 0; sfx_file("charge.wav"); }
        break;
    case L_CHARGE:
        if (frand(s) < dt * 30) {   /* sparks drawn into the mouth */
            Fx *e = fx_new(s);
            if (e) { float a = frand(s) * 2 * PI, r = 26 + frand(s) * 20; e->kind = FX_PULL; e->x = boss_ex(s) + cosf(a) * r; e->y = boss_ey(s) + sinf(a) * r; e->dur = 0.3f; e->vx = -cosf(a) * r / 0.3f; e->vy = -sinf(a) * r / 0.3f; }
        }
        if (b->laser_t >= 1.3f) { b->laser = L_FIRE; b->laser_t = 0; b->beam_len = 0; sfx_file("beam.wav"); s->shake = fmaxf(s->shake, 0.25f); }
        break;
    case L_FIRE: {
        b->beam_len = fminf(boss_ex(s) + 8, b->beam_len + 1500 * dt);
        float dur = b->stage >= 2 ? 2.6f : 1.5f;
        if (fabsf(s->py - boss_ey(s)) < beam_half(s) + 1 && s->px > boss_ex(s) - b->beam_len - 20 && s->px < boss_ex(s)) player_hurt(s, "beam");
        if (b->laser_t >= dur) { b->laser = L_FADE; b->laser_t = 0; }
        break; }
    case L_FADE:
        if (b->laser_t >= 0.25f) { b->laser = L_IDLE; b->laser_cd = (b->stage == 0 ? 8.5f : b->stage == 1 ? 7.0f : 5.5f) / (0.7f + 0.3f * k); }
        break;
    }
    /* the hangar: a swarm of drones out of the bay */
    b->swarm_cd -= dt;
    if (b->swarm_cd <= 0 && b->swarm_left == 0) { boss_launch_swarm(s); b->swarm_cd = b->stage == 0 ? 11.0f : b->stage == 1 ? 9.0f : 8.0f; }
    if (b->swarm_left > 0) {
        b->swarm_t -= dt;
        if (b->swarm_t <= 0) {
            Foe *d = foe_new(s);
            if (d) {
                d->kind = F_DRONE; d->pat = P_SNAKE; d->group = b->swarm_group; d->hp = 2 * hpmul(s);
                d->x = b->x + b->bayx; d->y = d->y0 = b->y + b->bayy; d->t = (b->swarm_n - b->swarm_left) * 0.25f; d->fire_t = 1e9f;
                if (b->swarm_group >= 0) s->group[b->swarm_group].alive++;
                spawn_fx(s, FX_FLASH, d->x, d->y, 0.8f, 0.1f);
            }
            b->swarm_left--; b->swarm_t = 0.2f;
        }
    }
    /* the mine rack (under two thirds): pairs of mines rolled off the blade */
    if (b->stage >= 1) {
        b->mine_cd -= dt;
        if (b->mine_cd <= 0) {
            for (int j = 0; j < 2; j++) {
                Foe *m = foe_new(s); if (!m) break;
                m->kind = F_MINE; m->pat = P_LAUNCH; m->x = b->x + b->rackx; m->y = b->y + b->racky; m->vx = -110; m->vy = j ? 55 : -35;
                m->laser = b->stage >= 2 || j == 0; m->cool = 1.5f; m->t = frand(s) * 6;
            }
            b->mine_cd = b->stage == 1 ? 7.5f : 6.5f; sfx_file("launch.wav");
        }
    }
    /* battle damage */
    if (b->stage >= 2 && (b->burn_t -= dt) <= 0) {
        b->burn_t = 0.35f;
        for (int tries = 0; tries < 20; tries++) {
            float x = frand(s) * s->boss_w, y = frand(s) * s->boss_h;
            if (boss_solid(s, b->x + x, b->y + y)) { spawn_expl(s, b->x + x, b->y + y, 0.35f); if (frand(s) < 0.5f) spawn_fx(s, FX_SMOKE, b->x + x, b->y + y, 0.7f, 1.0f); break; }
        }
    }
}

static void boss_die_update(Space *s, float dt)
{
    Boss *b = &s->boss;
    b->die_t += dt;
    if (!b->broken) {
        b->x += 6 * dt; b->y += 9 * dt; b->tilt = fminf(4.0f, b->tilt + dt * 0.9f);
        b->expl_t -= dt;
        if (b->expl_t <= 0) {
            b->expl_t = 0.07f;
            for (int tries = 0; tries < 30; tries++) {
                float x = frand(s) * s->boss_w, y = frand(s) * s->boss_h;
                if (boss_solid(s, b->x + x, b->y + y)) { spawn_expl(s, b->x + x, b->y + y, 0.6f + frand(s) * 0.9f); break; }
            }
            if (frand(s) < 0.4f) sfx_play(frand(s) < 0.5f ? 5 : 6, 0);
        }
        s->shake = fmaxf(s->shake, 0.2f);
        if (((int)(b->die_t * 10)) % 7 == 0) b->flash = 0.05f;
        if (b->die_t >= 4.6f) {   /* the reactor goes: the hull breaks into pieces */
            b->broken = true; s->white = 1.2f; s->shake = 1.4f;
            sfx_play(0x15, 0); sfx_play(6, 3); sfx_play(5, 8);
            float cx = b->x + s->boss_w * 0.5f, cy = b->y + s->boss_h * 0.5f;
            const int C = 16;
            for (int gy = 0; gy < s->boss_h; gy += C) for (int gx = 0; gx < s->boss_w; gx += C) {
                int solid = 0;
                for (int y = gy; y < gy + C && y < s->boss_h; y++) for (int x = gx; x < gx + C && x < s->boss_w; x++) solid += (s->boss_px[y * s->boss_w + x] >> 24) > 0;
                if (solid < 24) continue;
                Fx *e = fx_new(s); if (!e) break;
                float x = b->x + gx + C * 0.5f, y = b->y + gy + C * 0.5f, dx = x - cx, dy = y - cy, d = hypotf(dx, dy) + 1;
                e->kind = FX_CHUNK; e->x = x; e->y = y; e->src = (SDL_FRect){ (float)gx, (float)gy, (float)C, (float)C };
                float v = 30 + frand(s) * 90;
                e->vx = dx / d * v + 15; e->vy = dy / d * v * 0.8f + (frand(s) - 0.5f) * 30; e->vr = (frand(s) - 0.5f) * 200; e->rot = b->tilt;
                e->dur = 3.0f + frand(s) * 1.5f;
            }
            for (int k = 0; k < 16; k++) spawn_expl(s, b->x + frand(s) * s->boss_w, b->y + 20 + frand(s) * (s->boss_h - 40), 1.2f + frand(s) * 1.4f);
            music_play(6, false); s->music_now = 6;
        }
    } else if ((int)((b->die_t - dt) * 4) != (int)(b->die_t * 4) && b->die_t < 7) {
        spawn_expl(s, b->x + frand(s) * s->boss_w, b->y + frand(s) * s->boss_h, 0.8f + frand(s));
    }
}

/* ---------------------------------------------------------------- shots, pickups, effects */
static void update_shots(Space *s, float dt)
{
    Boss *b = &s->boss;
    s->clink_t -= dt;
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *sh = &s->shot[i]; if (!sh->on) continue;
        sh->x += sh->vx * dt; sh->y += sh->vy * dt; sh->life -= dt;
        if (sh->kind == S_TORPEDO) {
            bool hit = sh->life <= 0 || boss_solid(s, sh->x + 5, sh->y);
            for (int j = 0; j < MAX_FOE && !hit; j++) { Foe *f = &s->foe[j]; if (f->kind != F_OFF && f->wait <= 0 && in_box(f, sh->x, sh->y, 4)) hit = true; }
            if (hit) { sh->on = false; detonate_torpedo(s, sh->x, sh->y); }
            continue;
        }
        if (sh->life <= 0 || sh->x < -70 || sh->x > s->sw + 20 || sh->y < -10 || sh->y > s->sh + 10) { sh->on = false; continue; }
        if (sh->kind == S_BOLT) {
            float fx = sh->x + 5;
            for (int j = 0; j < MAX_FOE && sh->on; j++) {
                Foe *f = &s->foe[j]; if (f->kind == F_OFF || f->wait > 0) continue;
                if (!in_box(f, fx, sh->y, f->kind == F_MINE ? 3 : 1)) continue;
                sh->on = false;
                if (f->kind == F_MINE) {   /* armoured: the bolt glances off */
                    spawn_fx(s, FX_SPARK, fx - 3, sh->y, 1, 0.12f);
                    if (s->clink_t <= 0) { sfx_play(13, 0); s->clink_t = 0.25f; }
                } else {
                    f->hp -= 1; f->flash = 0.08f; spawn_fx(s, FX_SPARK, fx, sh->y, 1, 0.1f);
                    if (f->hp <= 0) foe_kill(s, f, true); else sfx_play(14, 0);
                }
            }
            if (sh->on && boss_solid(s, fx, sh->y)) {
                sh->on = false; spawn_fx(s, FX_SPARK, fx - 2, sh->y, 1, 0.1f);
                if (s->phase == PH_BOSS) {
                    bool port = false;
                    for (int k = 0; k < b->nports; k++) {
                        Port *p = &b->port[k];
                        if (!p->lit || p->dead || hypotf(fx - (b->x + p->x), sh->y - (b->y + p->y)) > 6.5f) continue;
                        port = true; p->hp -= 1; b->flash = 0.05f;
                        if (p->hp <= 0) { p->dead = true; blast(s, b->x + p->x, b->y + p->y, 0.8f); b->hp -= 30; sfx_play(6, 0); }
                        break;
                    }
                    if (!port) { b->hp -= 1; b->flash = 0.05f; }
                    if (s->clink_t <= 0) { sfx_play(14, 0); s->clink_t = 0.12f; }
                }
            }
        } else if (alive(s)) {
            bool hit;
            if (sh->kind == S_MLASER) hit = s->px > sh->x - 2 && s->px < sh->x + 59 && fabsf(s->py - sh->y) < 3 + PL_R;
            else { float dx = sh->x - s->px, dy = sh->y - s->py; hit = dx * dx + dy * dy < (3 + PL_R) * (3 + PL_R); }
            if (hit && vulnerable(s)) { sh->on = false; player_hurt(s, sh->kind == S_MLASER ? "mine laser" : "orb"); }
        }
    }
    /* Ramrod against ships and mines */
    if (vulnerable(s)) {
        for (int j = 0; j < MAX_FOE; j++) {
            Foe *f = &s->foe[j]; if (f->kind == F_OFF || f->wait > 0) continue;
            if (in_box(f, s->px, s->py, f->kind == F_MINE ? PL_R : 2)) {
                player_hurt(s, f->kind == F_MINE ? "mine" : "ram");
                if (f->kind == F_MINE) foe_kill(s, f, false);
                else { f->hp -= 20; if (f->hp <= 0) foe_kill(s, f, true); }
                break;
            }
        }
    }
}

static void update_caps(Space *s, float dt)
{
    for (int i = 0; i < MAX_CAP; i++) {
        Cap *c = &s->cap[i]; if (!c->on) continue;
        c->t += dt; c->x -= 32 * dt; c->y += sinf(c->t * 3) * 14 * dt;
        if (c->x < -20) { c->on = false; continue; }
        if (alive(s) && fabsf(c->x - s->px) < 16 && fabsf(c->y - s->py) < 12) {
            c->on = false; sfx_file("pickup.wav");
            if (c->kind == CAP_P) {
                if (s->power < 3) { s->power++; set_msg(s, s->power == 3 ? "MAX POWER" : "POWER UP", NULL, 1.2f); }
                else { s->bombs = s->bombs < 5 ? s->bombs + 1 : 5; set_msg(s, "TORPEDO +1", NULL, 1.2f); }
                if (!s->power_said) { s->power_said = true; radio(s, R_POWER); }
            } else if (c->kind == CAP_S) {
                s->hp = s->hp < s->hp_max ? s->hp + 1 : s->hp_max; s->crit_said = false; set_msg(s, "SHIELD REPAIRED", NULL, 1.2f);
                if (!s->shield_said) { s->shield_said = true; radio(s, R_SHIELD); }
            } else { s->bombs = s->bombs < 5 ? s->bombs + 1 : 5; set_msg(s, "TORPEDO +1", NULL, 1.2f); }
        }
    }
}

static void update_fx(Space *s, float dt)
{
    for (int i = 0; i < MAX_FX; i++) {
        Fx *e = &s->fx[i]; if (!e->on) continue;
        e->t += dt; e->x += e->vx * dt; e->y += e->vy * dt; e->rot += e->vr * dt;
        if (e->kind == FX_SMOKE) e->scale += dt * 0.7f;
        if (e->kind == FX_CHUNK && frand(s) < dt * 3) spawn_fx(s, FX_SMOKE, e->x, e->y, 0.5f, 0.8f);
        if (e->t >= e->dur) e->on = false;
    }
}

static void update_scenery(Space *s, float dt)
{
    static const float SPD[3] = { 16, 38, 85 };
    s->neb_x += 9 * dt; s->planet_x -= 8 * dt;
    for (int i = 0; i < N_STAR; i++) {
        s->star_x[i] -= SPD[s->star_l[i]] * dt;
        if (s->star_x[i] < -4) { s->star_x[i] += s->sw + 8; s->star_y[i] = frand(s) * s->sh; }
    }
    if (s->far_on) { s->far_x -= 19 * dt; if (s->far_x < -s->far_w - 10) s->far_on = false; }
}

/* debug (SABER_R7BOT=1): an autopilot for flow tests - fires all the time, picks the safest of nine moves by
 * looking a quarter second ahead, lines up with the nearest ship, stays out of the beam */
static void bot_input(Space *s, Input *out)
{
    static int n; n++;
    for (int b = 0; b < BTN_COUNT; b++) out->state[b] = 1;
    out->state[BTN_SHOOT] = (n % 10) ? 0 : 1;   /* let go now and then: fire_hold waits for a release after a scene */
    float best = 1e9f; int bdx = 0, bdy = 0;
    float ty = s->py;
    { float bd = 1e9f; for (int i = 0; i < MAX_FOE; i++) { Foe *f = &s->foe[i]; if (f->kind == F_OFF || f->kind == F_MINE || f->wait > 0 || f->x < s->px) continue; float d = f->x - s->px; if (d < bd) { bd = d; ty = f->y; } } }
    if (s->boss.on && s->phase == PH_BOSS) ty = boss_ey(s) + (s->boss.laser != L_IDLE ? 40 : 0);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        float x = s->px + dx * PL_SPEED * 0.25f, y = s->py + dy * PL_SPEED * 0.25f, cost = 0;
        if (y < TOP + 4 || y > s->sh - 14 || x < 20 || x > s->sw * 0.45f) cost += 500;
        for (int i = 0; i < MAX_SHOT; i++) {
            Shot *sh = &s->shot[i]; if (!sh->on || sh->kind < S_ORB) continue;
            for (float t = 0; t <= 0.3f; t += 0.1f) {
                float sx = sh->x + sh->vx * t, sy = sh->y + sh->vy * t, ex = sh->kind == S_MLASER ? 30 : 0;
                float d = hypotf(fmaxf(0, fabsf(sx + ex - x) - ex), sy - y); if (d < 14) cost += (14 - d) * 20;
            }
        }
        for (int i = 0; i < MAX_FOE; i++) {
            Foe *f = &s->foe[i]; if (f->kind == F_OFF || f->wait > 0) continue;
            for (float t = 0; t <= 0.4f; t += 0.1f) { float d = hypotf(f->x + f->vx * t - x, f->y + f->vy * t - y); float r = f->kind == F_MINE ? 34 : 24; if (d < r) cost += (r - d) * 15; }
        }
        if (s->boss.on && s->boss.laser != L_IDLE && fabsf(y - boss_ey(s)) < beam_half(s) + 16) cost += 800;
        if (boss_solid(s, x + 30, y)) cost += 300;
        cost += fabsf(y - ty) * 0.3f + fabsf(x - 70) * 0.05f;
        if (cost < best) { best = cost; bdx = dx; bdy = dy; }
    }
    if (bdx < 0) out->state[BTN_LEFT] = 0;
    if (bdx > 0) out->state[BTN_RIGHT] = 0;
    if (bdy < 0) out->state[BTN_UP] = 0;
    if (bdy > 0) out->state[BTN_DOWN] = 0;
    int near = 0; for (int i = 0; i < MAX_SHOT; i++) if (s->shot[i].on && s->shot[i].kind >= S_ORB && hypotf(s->shot[i].x - s->px, s->shot[i].y - s->py) < 40) near++;
    if (near >= 4 && (n % 30) == 0) out->state[BTN_JUMP] = 2;
}

static int foes_alive(const Space *s) { int n = 0; for (int i = 0; i < MAX_FOE; i++) if (s->foe[i].kind != F_OFF && s->foe[i].kind != F_MINE) n++; return n; }

static void update_world(Space *s, const Input *in, float dt, bool control)
{
    Input bot;
    if (control && s->bot) { bot_input(s, &bot); in = &bot; }
    if (control) player_control(s, in, dt);
    if (s->inv_t > 0) s->inv_t -= dt;
    if (s->dead) {
        s->dead_t += dt;
        if (s->dead_t >= 2.2f && s->phase != PH_GAMEOVER) {
            if (s->lives <= 0) { s->phase = PH_GAMEOVER; s->phase_t = 0; music_stop(); s->music_now = -1; }
            else {   /* a spare: Ramrod flies back in where the fight is */
                s->lives--; s->dead = false; s->hp = s->hp_max; s->crit_said = false; s->bombs = s->bombs < 3 ? 3 : s->bombs;
                s->px = -30; s->py = s->sh * 0.5f; s->enter_t = 0.9f; s->inv_t = 3.0f;
            }
        }
    }
    for (int i = 0; i < MAX_FOE; i++) if (s->foe[i].kind != F_OFF) foe_update(s, &s->foe[i], dt);
    if (s->boss.on && s->phase != PH_BOSS_DIE) boss_update(s, dt);
    update_shots(s, dt);
    update_caps(s, dt);
    update_fx(s, dt);
    update_scenery(s, dt);
}

/* ---------------------------------------------------------------- flow */
void space_update(Space *s, const Input *in, float dt)
{
    if (!s->ok) { s->result = 1; return; }
    if (s->result) return;
    if (power_in_cutin(&s->pw)) {   /* the power attack's cut-in: everything holds still under it */
        power_update(&s->pw, in, dt);
        if (power_take_strike(&s->pw)) hero_bomb(s);
        return;
    }
    power_update(&s->pw, in, dt);
    bool can_pause = s->phase == PH_PLAY || s->phase == PH_BOSS || s->phase == PH_BOSS_IN || s->phase == PH_WARNING;
    if (btn_pressed(in, BTN_PAUSE) && can_pause && !s->dlg.active) { s->paused = !s->paused; sfx_play(10, 0); music_pause(s->paused); }
    if (s->paused) return;
    s->phase_t += dt; s->total_t += dt;
    if (s->msg_t > 0) s->msg_t -= dt;
    if (s->hurt_t > 0) s->hurt_t -= dt;
    if (s->shake > 0) s->shake -= dt;
    s->red = fmaxf(0, s->red - dt * 1.6f); s->white = fmaxf(0, s->white - dt * 1.4f);
    /* the radio: one line at a time along the bottom, over the fight */
    if (s->radio_now >= 0) { s->radio_t += dt; if (s->radio_t > 4.2f) s->radio_now = -1; }
    if (s->radio_now < 0 && s->radio_n > 0 && !s->dlg.active) {
        s->radio_now = s->radio_q[0]; s->radio_t = 0; memmove(s->radio_q, s->radio_q + 1, sizeof(int) * (size_t)(--s->radio_n));
        sfx_play(0, 0);
    }
    { static int last = -1; if (trace() && s->phase != last) { fprintf(stderr, "r7 phase %d t=%.1f tl=%.1f hp=%d lives=%d\n", s->phase, s->total_t, s->tl_t, s->hp, s->lives); last = s->phase; } }
    if (trace() && (int)s->total_t != (int)(s->total_t - dt))
        fprintf(stderr, "r7 t=%.0f tl=%.1f ph=%d hp=%d lives=%d pow=%d bombs=%d foes=%d boss=%.0f/%d pos=%.0f,%.0f\n", s->total_t, s->tl_t, s->phase, s->hp, s->lives, s->power, s->bombs, foes_alive(s), s->boss.hp, s->boss.stage, s->px, s->py);

    switch (s->phase) {
    case PH_INSTR: {
        update_scenery(s, dt);
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        if (any && s->phase_t >= INSTR_OPEN) s->phase_t = INSTR_MIN;
        if (s->phase_t >= INSTR_MIN) { s->phase = PH_PLAY; s->phase_t = 0; s->fire_hold = s->bomb_hold = true; set_msg(s, "RAMROD - CRUISER MODE", "FULL THRUST!", 2.0f); }
        break; }
    case PH_PLAY:
        s->tl_t += dt;
        while (s->tl_next < N_TL && s->tl[s->tl_next].t <= s->tl_t) run_event(s, &s->tl[s->tl_next++]);
        update_world(s, in, dt, true);
        if (s->tl_t >= PRE_BOSS && (foes_alive(s) == 0 || s->tl_t >= PRE_BOSS + 6)) {
            s->phase = PH_WARNING; s->phase_t = 0; music_stop(); s->music_now = -1; sfx_file("warning.wav");
        }
        break;
    case PH_WARNING:
        update_world(s, in, dt, true);
        if ((int)(s->phase_t / 1.0f) != (int)((s->phase_t - dt) / 1.0f) && s->phase_t < WARN_DUR - 0.5f) sfx_file("warning.wav");
        if (s->phase_t >= WARN_DUR) { s->phase = PH_BOSS_IN; s->phase_t = 0; boss_start(s); play_music(s, 11); }
        break;
    case PH_BOSS_IN:
        update_world(s, in, dt, true);
        if (s->phase_t >= BOSS_IN_DUR) {
            s->phase = PH_BOSS_TALK; s->phase_t = 0;
            if (!s->bot) dialog_open_script(&s->dlg, SCRIPT_BOSS);   /* the autopilot skips the scenes */
        }
        break;
    case PH_BOSS_TALK:
        update_fx(s, dt); update_scenery(s, dt);
        if (s->dlg.active) dialog_update(&s->dlg, in, dt);
        else { s->phase = PH_BOSS; s->phase_t = 0; s->fire_hold = s->bomb_hold = true; }
        break;
    case PH_BOSS:
        update_world(s, in, dt, true);
        if (s->boss.hp <= 0 && s->phase == PH_BOSS) {
            s->phase = PH_BOSS_DIE; s->phase_t = 0; s->boss.hp = 0; s->boss.laser = L_IDLE; music_stop(); s->music_now = -1;
            for (int i = 0; i < MAX_SHOT; i++) if (s->shot[i].on && s->shot[i].kind >= S_ORB) s->shot[i].on = false;
            for (int i = 0; i < MAX_FOE; i++) if (s->foe[i].kind != F_OFF && s->foe[i].wait <= 0) foe_kill(s, &s->foe[i], false); else s->foe[i].kind = F_OFF;
            s->inv_t = 99;   /* nothing can touch Ramrod now */
            sfx_play(0x11, 0);
        }
        break;
    case PH_BOSS_DIE:
        update_world(s, in, dt, true);
        boss_die_update(s, dt);
        if (s->phase_t >= 8.5f) { s->phase = PH_OUTRO; s->phase_t = 0; if (!s->bot) dialog_open_script(&s->dlg, SCRIPT_OUTRO); }
        break;
    case PH_OUTRO:
        update_fx(s, dt); update_scenery(s, dt);
        if (s->dlg.active) dialog_update(&s->dlg, in, dt);
        else { s->phase = PH_CLEARED; s->phase_t = 0; }
        break;
    case PH_CLEARED:
        update_scenery(s, dt); s->px += 160 * dt;   /* Ramrod turns for home, off to the right... */
        if (s->phase_t > 1.6f) s->result = 1;
        break;
    case PH_GAMEOVER:
        update_fx(s, dt);
        s->black = clampf(s->phase_t / 1.0f, 0, 1);
        if (s->phase_t > 1.4f) s->result = 2;
        break;
    }
}

/* ---------------------------------------------------------------- drawing */
static void draw_frame(Space *s, int a, int fr, float x, float y, float scale, bool flip, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    Anim *an = &s->anim[a]; if (an->n == 0) return;
    fr = fr < 0 ? 0 : fr >= an->n ? an->n - 1 : fr;
    Frame *f = &an->f[fr];
    SDL_FRect src = { (float)f->x, (float)f->y, (float)f->w, (float)f->h };
    float ax = flip ? f->w - 1 - f->ax : f->ax;
    SDL_FRect dst = { floorf(x - ax * scale), floorf(y - f->ay * scale), roundf(f->w * scale), roundf(f->h * scale) };
    if (dst.w < 1 || dst.h < 1) return;
    SDL_SetTextureColorMod(s->atlas, cr, cg, cb); SDL_SetTextureAlphaMod(s->atlas, ca);
    SDL_RenderTextureRotated(s->ren, s->atlas, &src, &dst, 0, NULL, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureColorMod(s->atlas, 255, 255, 255); SDL_SetTextureAlphaMod(s->atlas, 255);
}
static void draw_add(Space *s, int a, int fr, float x, float y, float scale, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    SDL_SetTextureBlendMode(s->atlas, SDL_BLENDMODE_ADD);
    draw_frame(s, a, fr, x, y, scale, false, cr, cg, cb, ca);
    SDL_SetTextureBlendMode(s->atlas, SDL_BLENDMODE_BLEND);
}
static void rect(SDL_Renderer *ren, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(ren, r, g, b, a); SDL_FRect q = { floorf(x), floorf(y), w, h }; SDL_RenderFillRect(ren, &q);
}
static void fill_circle(SDL_Renderer *ren, float cx, float cy, float rad, SDL_FColor c)
{
    enum { N = 24 }; SDL_Vertex v[N + 1]; int idx[N * 3];
    v[0].position = (SDL_FPoint){ cx, cy }; v[0].color = c;
    for (int i = 0; i < N; i++) { float a = i * 2 * PI / N; v[i + 1].position = (SDL_FPoint){ cx + cosf(a) * rad, cy + sinf(a) * rad }; v[i + 1].color = c; }
    for (int i = 0; i < N; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = 1 + i; idx[i * 3 + 2] = 1 + (i + 1) % N; }
    SDL_RenderGeometry(ren, NULL, v, N + 1, idx, N * 3);
}

static void render_background(Space *s)
{
    SDL_Renderer *ren = s->ren;
    float u = fmodf(s->neb_x, (float)NEB_W);
    for (float x = -u; x < s->sw; x += NEB_W) { SDL_FRect d = { floorf(x), 0, NEB_W, 240 }; SDL_RenderTexture(ren, s->neb, NULL, &d); }
    for (int i = 0; i < N_STAR; i++) if (s->star_l[i] == 0) rect(ren, s->star_x[i], s->star_y[i], 1, 1, 110, 110, 170, 255);
    if (s->far_on) {
        SDL_FRect d = { floorf(s->far_x), floorf(s->far_y), (float)s->far_w, (float)s->far_h };
        SDL_RenderTexture(ren, s->far, NULL, &d);
    }
    if (s->planet_x - 120 > -s->planet_w) {   /* Yuma, falling behind */
        SDL_FRect d = { floorf(s->planet_x - 120), (float)(s->sh - 150), (float)s->planet_w, (float)s->planet_h };   /* the disc's centre 120 px left of the screen, 150 below it */
        SDL_RenderTexture(ren, s->planet, NULL, &d);
    }
    for (int i = 0; i < N_STAR; i++) {
        if (s->star_l[i] == 1) rect(ren, s->star_x[i], s->star_y[i], 1, 1, 190, 190, 255, 255);
        else if (s->star_l[i] == 2) {
            if (i % 5 == 0) draw_frame(s, A_STAR, (i / 5) % 3, s->star_x[i], s->star_y[i], 1, false, 255, 255, 255, 220);
            else { rect(ren, s->star_x[i] - 2, s->star_y[i], 3, 1, 150, 160, 255, 160); rect(ren, s->star_x[i], s->star_y[i], 1, 1, 255, 255, 255, 255); }
        }
    }
}

static void render_boss(Space *s)
{
    Boss *b = &s->boss; SDL_Renderer *ren = s->ren;
    if (!b->on || b->broken) return;
    SDL_FRect d = { floorf(b->x), floorf(b->y), (float)s->boss_w, (float)s->boss_h };
    SDL_FPoint piv = { (float)s->boss_w, s->boss_h * 0.5f };
    uint8_t v = 255;
    if (s->phase == PH_BOSS_DIE) { v = (uint8_t)(255 - clampf(b->die_t / 4.6f, 0, 1) * 110); }
    SDL_SetTextureColorMod(s->boss_tex, v, v, v);
    SDL_RenderTextureRotated(ren, s->boss_tex, NULL, &d, -b->tilt, &piv, SDL_FLIP_NONE);
    if (b->flash > 0) {
        SDL_SetTextureBlendMode(s->boss_tex, SDL_BLENDMODE_ADD); SDL_SetTextureColorMod(s->boss_tex, 120, 90, 70);
        SDL_RenderTextureRotated(ren, s->boss_tex, NULL, &d, -b->tilt, &piv, SDL_FLIP_NONE);
        SDL_SetTextureBlendMode(s->boss_tex, SDL_BLENDMODE_BLEND);
    }
    SDL_SetTextureColorMod(s->boss_tex, 255, 255, 255);
    /* the gun ports: lit, dark, wrecked; a live one pulses before it fires. They follow the hull as it lists. */
    float a = -b->tilt * PI / 180, ca = cosf(a), sa = sinf(a);
    for (int i = 0; i < b->nports; i++) {
        Port *p = &b->port[i];
        int fr = p->dead ? 2 : p->lit ? 0 : 1;
        Frame *f = &s->anim[A_PORT].f[fr];
        float dx = roundf(p->x - 5 + 0.01f) + 5 - piv.x, dy = roundf(p->y - 5 + 0.01f) + 5 - piv.y;
        float rx = dx * ca - dy * sa + piv.x - 5, ry = dx * sa + dy * ca + piv.y - 5;
        SDL_FRect src = { (float)f->x, (float)f->y, (float)f->w, (float)f->h };
        SDL_FRect dst = { floorf(b->x) + roundf(rx), floorf(b->y) + roundf(ry), (float)f->w, (float)f->h };
        SDL_SetTextureColorMod(s->atlas, v, v, v);
        SDL_RenderTexture(ren, s->atlas, &src, &dst);
        SDL_SetTextureColorMod(s->atlas, 255, 255, 255);
        if (p->lit && !p->dead && p->fire_t < 0.4f && s->phase == PH_BOSS) draw_add(s, A_ORB, 1, b->x + p->x, b->y + p->y, 0.8f + (0.4f - p->fire_t), 255, 255, 255, 200);
    }
}

static void render_beam(Space *s)
{
    Boss *b = &s->boss; SDL_Renderer *ren = s->ren;
    if (!b->on || b->broken || s->phase == PH_BOSS_DIE) return;
    float ex = boss_ex(s), ey = boss_ey(s);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (b->laser == L_CHARGE) {
        float p = clampf(b->laser_t / 1.3f, 0, 1);
        if (p > 0.5f && ((int)(b->laser_t * 16) & 1)) rect(ren, 0, ey, ex, 1, 230, 0, 40, 255);   /* the aiming line */
        fill_circle(ren, ex, ey, 1.5f + 6 * p, (SDL_FColor){ 1, 0.6f, 0.62f, 1 });
        fill_circle(ren, ex, ey, 0.8f + 4 * p, (SDL_FColor){ 1, 0.95f, 0.95f, 1 });
    } else if (b->laser == L_FIRE || b->laser == L_FADE) {
        /* the clip's profile: dark red edge, pink, a white core (11 rows; doubled in the last stage) */
        float k = b->stage >= 2 ? 2.0f : 1.0f, fl = ((int)(s->total_t * 30) & 1) ? 1.0f : 0.0f;
        if (b->laser == L_FADE) k *= 1 - clampf(b->laser_t / 0.25f, 0, 1);
        float x0 = ex - b->beam_len, w = b->beam_len;
        float H = 5.5f * k;
        rect(ren, x0, ey - H - fl * 0.5f, w, 2 * H + fl, 175, 0, 34, 255);
        rect(ren, x0, ey - H * 0.82f, w, 2 * H * 0.82f, 251, 153, 151, 255);
        rect(ren, x0, ey - H * 0.46f - fl * 0.5f, w, 2 * H * 0.46f + fl, 249, 232, 232, 255);
        float br = (b->stage >= 2 ? 18 : 11) * (b->laser == L_FADE ? k / 2 + 0.5f : 1) + fl;
        fill_circle(ren, ex, ey, br, (SDL_FColor){ 0.98f, 0.55f, 0.58f, 1 });
        fill_circle(ren, ex, ey, br - 2, (SDL_FColor){ 1, 0.97f, 0.97f, 1 });
    }
}

static void render_foes(Space *s)
{
    for (int i = 0; i < MAX_FOE; i++) {
        Foe *f = &s->foe[i]; if (f->kind == F_OFF || f->wait > 0) continue;
        switch (f->kind) {
        case F_MINE: {
            uint8_t r = 255, g = 255, b = 255;
            if (f->pat == P_SEEK) { g = 220; b = 150; }
            if (f->charge > 0 && ((int)(f->charge * 20) & 1)) { g = 150; b = 150; }
            draw_frame(s, A_MINE, 0, f->x, f->y + sinf(f->t * 2.1f), 1, false, r, g, b, 255);
            if (f->laser) {   /* the barrel glows: brighter while it charges */
                float p = f->charge > 0 ? 0.5f + f->charge / 0.65f : 0.3f + 0.15f * sinf(f->t * 5);
                draw_add(s, A_TIP, 0, f->x - 22, f->y - 1, clampf(p, 0.3f, 1.6f), 255, 255, 255, (uint8_t)clampf(160 + 90 * p, 0, 255));
            }
            break; }
        case F_FIGHTER: case F_DRONE: case F_GUNSHIP: {
            int a = f->kind == F_FIGHTER ? A_FIGHTER : f->kind == F_DRONE ? A_DRONE : A_GUNSHIP;
            bool flip = f->pat == P_REAR || (f->pat == P_SWOOP && f->vx > 0);
            Frame *fr = &s->anim[a].f[0];
            float tail = flip ? f->x - fr->w * 0.5f : f->x + fr->w * 0.5f - (a == A_GUNSHIP ? 4 : 2);
            float fl = 2 + ((int)(s->total_t * 24 + i) & 1) * 2;   /* the exhaust */
            rect(s->ren, flip ? tail - fl : tail, f->y - 1 + (a == A_GUNSHIP ? 1 : 0), fl, 2, 200, 120, 255, 220);
            draw_frame(s, a, 0, f->x, f->y, 1, flip, 255, 255, 255, 255);
            if (f->flash > 0) draw_add(s, a, 0, f->x, f->y, 1, 255, 255, 255, 180);
            break; }
        }
    }
    /* rear attack warning: chevrons on the left edge while they wait to come in */
    for (int i = 0; i < MAX_FOE; i++) {
        Foe *f = &s->foe[i];
        if (f->kind == F_OFF || f->pat != P_REAR || f->wait <= 0 || f->wait > 1.3f || ((int)(s->total_t * 8) & 1)) continue;
        for (int k = 0; k < 5; k++) { float w = (float)(5 - k); rect(s->ren, 4 + k, f->y0 - w, 1, w * 2, 255, 80, 60, 255); }
    }
}

static void render_player(Space *s)
{
    if (s->dead || s->phase == PH_GAMEOVER) return;
    if (s->inv_t > 0 && s->inv_t < 50 && ((int)(s->inv_t * 16) & 1)) return;
    Frame *f = &s->anim[A_PLAYER].f[0];
    float tx = s->px - f->w * 0.5f + 2, ty = s->py + 1;
    float fl = 5 + ((int)(s->total_t * 30) & 1) * 3 + (s->phase == PH_CLEARED ? 8 : 0);   /* the engine flame */
    rect(s->ren, tx - fl, ty - 2, fl, 4, 255, 150, 40, 230);
    rect(s->ren, tx - fl * 0.6f, ty - 1, fl * 0.6f, 2, 255, 250, 200, 255);
    draw_frame(s, A_PLAYER, 0, s->px, s->py, 1, false, 255, 255, 255, 255);
    if (s->hurt_t > 0.3f) draw_add(s, A_PLAYER, 0, s->px, s->py, 1, 255, 120, 120, 200);
}

static void render_shots(Space *s)
{
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *sh = &s->shot[i]; if (!sh->on) continue;
        switch (sh->kind) {
        case S_BOLT: draw_add(s, A_BOLT, sh->vy != 0, sh->x, sh->y, 1, 255, 255, 255, 255); break;
        case S_TORPEDO: draw_add(s, A_TORPEDO, (int)(s->total_t * 20) & 1, sh->x, sh->y, 1, 255, 255, 255, 255); rect(s->ren, sh->x - 12, sh->y - 1, 8, 2, 255, 200, 90, 200); break;
        case S_ORB: draw_frame(s, A_ORB, (int)(s->total_t * 12 + i) & 1, sh->x, sh->y, 1, false, 255, 255, 255, 255); break;
        case S_MLASER: draw_frame(s, A_MLASER, 0, sh->x, sh->y, 1, false, 255, 255, 255, 255); break;
        }
    }
    for (int i = 0; i < MAX_CAP; i++) {
        Cap *c = &s->cap[i]; if (!c->on) continue;
        draw_frame(s, A_CAP, c->kind - 1, c->x, c->y, 1, false, 255, 255, 255, 255);
        if ((int)(c->t * 6) & 1) draw_add(s, A_CAP, c->kind - 1, c->x, c->y, 1, 120, 120, 120, 255);
    }
}

static void render_fx(Space *s, bool chunks)
{
    for (int i = 0; i < MAX_FX; i++) {
        Fx *e = &s->fx[i]; if (!e->on || (e->kind == FX_CHUNK) != chunks) continue;
        float p = e->t / e->dur;
        switch (e->kind) {
        case FX_EXPL: draw_frame(s, A_EXPL, (int)(p * 6), e->x, e->y, e->scale, false, 255, 255, 255, 255); break;
        case FX_SMOKE: draw_frame(s, A_SMOKE, (int)(p * 4), e->x, e->y, e->scale, false, 150, 140, 150, (uint8_t)(170 * (1 - p))); break;
        case FX_FLASH: draw_add(s, A_FLASH, 0, e->x, e->y, e->scale, 255, 255, 255, 255); break;
        case FX_SPARK: draw_add(s, A_SPARK, p > 0.5f, e->x, e->y, 1, 255, 255, 255, 255); break;
        case FX_PULL: rect(s->ren, e->x, e->y, 2, 1, 255, 170, 190, 255); break;
        case FX_DEBRIS: {
            Frame *f = &s->anim[A_DEBRIS].f[e->frame % s->anim[A_DEBRIS].n];
            SDL_FRect src = { (float)f->x, (float)f->y, (float)f->w, (float)f->h }, dst = { floorf(e->x - f->w * 0.5f), floorf(e->y - f->h * 0.5f), (float)f->w, (float)f->h };
            SDL_SetTextureColorMod(s->atlas, 180, 120, 100); SDL_SetTextureAlphaMod(s->atlas, (uint8_t)(255 * (1 - clampf((p - 0.6f) / 0.4f, 0, 1))));
            SDL_RenderTextureRotated(s->ren, s->atlas, &src, &dst, e->rot, NULL, SDL_FLIP_NONE);
            SDL_SetTextureColorMod(s->atlas, 255, 255, 255); SDL_SetTextureAlphaMod(s->atlas, 255);
            break; }
        case FX_CHUNK: {
            SDL_FRect dst = { floorf(e->x - e->src.w * 0.5f), floorf(e->y - e->src.h * 0.5f), e->src.w, e->src.h };
            uint8_t v = (uint8_t)(200 - 120 * clampf(p * 1.5f, 0, 1));
            SDL_SetTextureColorMod(s->boss_tex, v, (uint8_t)(v * 0.8f), (uint8_t)(v * 0.7f));
            SDL_SetTextureAlphaMod(s->boss_tex, (uint8_t)(255 * (1 - clampf((p - 0.7f) / 0.3f, 0, 1))));
            SDL_RenderTextureRotated(s->ren, s->boss_tex, &e->src, &dst, e->rot, NULL, SDL_FLIP_NONE);
            SDL_SetTextureColorMod(s->boss_tex, 255, 255, 255); SDL_SetTextureAlphaMod(s->boss_tex, 255);
            break; }
        }
    }
}

static void center_text(Space *s, Font *f, const char *t, float y, uint8_t R, uint8_t G, uint8_t B)
{
    float x = s->sw * 0.5f - font_text_width(f, t) * 0.5f;
    font_draw(f, t, x + 1, y + 1, 0, 0, 0); font_draw(f, t, x, y, R, G, B);
}

/* the clip's HUD, top left: shield cells stacked red..green, then lives, gun power and torpedoes */
static void render_hud(Space *s, Font *small)
{
    SDL_Renderer *ren = s->ren;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    rect(ren, 3, 3, 82, fmaxf(58, 12 + s->hp_max * 9.0f), 0, 0, 20, 140);
    static const uint8_t CELL[5][3] = { { 90, 220, 60 }, { 170, 225, 50 }, { 250, 210, 40 }, { 250, 140, 30 }, { 230, 30, 30 } };
    static const int MAP[6][5] = { { 0 }, { 0 }, { 0, 4 }, { 0, 2, 4 }, { 0, 2, 3, 4 }, { 0, 1, 2, 3, 4 } };   /* green at the bottom, red on top */
    for (int i = 0; i < s->hp_max && i < 5; i++) {
        int c = MAP[s->hp_max][i];
        float y = 8 + (s->hp_max - 1 - i) * 9;
        bool on = i < s->hp;
        bool blink = s->hp == 1 && i == 0 && ((int)(s->total_t * 4) & 1);
        rect(ren, 7, y, 7, 7, 10, 10, 20, 255);
        if (on && !blink) { rect(ren, 7, y, 7, 7, CELL[c][0], CELL[c][1], CELL[c][2], 255); rect(ren, 8, y + 1, 2, 2, 255, 255, 255, 140); }
        else rect(ren, 8, y + 1, 5, 5, CELL[c][0] / 5, CELL[c][1] / 5, CELL[c][2] / 5, 255);
    }
    if (small) {
        char buf[16]; snprintf(buf, sizeof buf, "x%02d", s->lives);
        font_draw(small, "RAMROD", 19, 6, 255, 210, 90);
        font_draw(small, buf, 19, 16, 255, 255, 255);
        font_draw(small, "PWR", 19, 26, 150, 220, 255);
        font_draw(small, "TRP", 19, 36, 255, 190, 120);
        font_draw(small, "SPC", 19, 46, 255, 150, 220);
    }
    for (int i = 0; i < s->pw.items; i++) {   /* the hero's power attacks left: a little four-point star each */
        float x = 50 + i * 9.0f, y = 50;
        rect(ren, x - 1, y - 3, 3, 7, 255, 200, 240, 255); rect(ren, x - 3, y - 1, 7, 3, 255, 200, 240, 255); rect(ren, x, y, 1, 1, 255, 255, 255, 255);
    }
    power_draw_hud(&s->pw, ren, 48, 55, 26);
    for (int i = 0; i < 3; i++) rect(ren, 48 + i * 9, 28, 7, 5, i < s->power ? 80 : 20, i < s->power ? 230 : 40, i < s->power ? 255 : 60, 255);
    for (int i = 0; i < s->bombs; i++) draw_frame(s, A_TORPEDO, 0, 52 + i * 7, 40, 0.55f, false, 255, 255, 255, 255);
    /* the run to the cruiser */
    if (s->phase == PH_PLAY || s->phase == PH_INSTR) {
        float w = 110, x0 = s->sw * 0.5f - w * 0.5f, p = clampf(s->tl_t / PRE_BOSS, 0, 1);
        rect(ren, x0, 7, w, 1, 90, 90, 150, 200);
        for (int k = 0; k <= 4; k++) rect(ren, x0 + w * k / 4, 5, 1, 5, 90, 90, 150, 200);
        rect(ren, x0 + w - 3, 4, 6, 7, 170, 90, 50, 255);
        rect(ren, x0 + w * p - 2, 5, 5, 5, 120, 230, 255, 255);
    }
    /* the cruiser's hull */
    if (s->boss.on && (s->phase == PH_BOSS || s->phase == PH_BOSS_IN || s->phase == PH_BOSS_TALK)) {
        float w = 170, x0 = s->sw * 0.5f - w * 0.5f + 20, f = clampf(s->boss.hp / s->boss.hp_max, 0, 1);
        if (s->phase == PH_BOSS_IN) f *= clampf(s->phase_t / BOSS_IN_DUR, 0, 1);
        if (small) font_draw(small, "BATTLE CRUISER", x0, 4, 255, 170, 120);
        rect(ren, x0, 14, w, 5, 0, 0, 0, 200);
        rect(ren, x0, 14, floorf(w * f), 5, 230, 60 + (uint8_t)(120 * f), 40, 255);
        rect(ren, x0 + w * 0.33f, 14, 1, 5, 255, 255, 255, 120); rect(ren, x0 + w * 0.66f, 14, 1, 5, 255, 255, 255, 120);
    }
}

static int wrap_text(Font *f, const char *text, float width, char out[][96], int max)
{
    int n = 0; char line[96] = ""; const char *p = text;
    while (*p && n < max) {
        const char *e = p; while (*e && *e != ' ') e++;
        char word[64]; int wl = (int)(e - p); if (wl > 63) wl = 63; memcpy(word, p, (size_t)wl); word[wl] = 0;
        char trial[96]; bool fits = snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word) < (int)sizeof trial;
        if (line[0] && (!fits || font_text_width(f, trial) > width)) { snprintf(out[n++], 96, "%s", line); snprintf(line, sizeof line, "%s", word); }
        else memcpy(line, trial, sizeof line);
        p = *e ? e + 1 : e;
    }
    if (line[0] && n < max) snprintf(out[n++], 96, "%s", line);
    return n;
}

static void render_radio(Space *s, Font *small)
{
    if (s->radio_now < 0 || !small || s->dlg.active) return;
    const Radio *r = &RADIO[s->radio_now];
    float t = s->radio_t, in = clampf(t / 0.2f, 0, 1) * clampf((4.2f - t) / 0.2f, 0, 1);
    float w = fminf(300, s->sw - 20.0f), x0 = s->sw * 0.5f - w * 0.5f, h = 30, y0 = s->sh - 4 - h * in;
    SDL_SetRenderDrawBlendMode(s->ren, SDL_BLENDMODE_BLEND);
    rect(s->ren, x0, y0, w, h, 8, 14, 40, 210);
    rect(s->ren, x0, y0, w, 1, 90, 160, 255, 255);
    Sprite *av = sprite_get(namehash(r->avatar));
    if (av) sprite_draw(av, 0, x0 + 1, y0 - 3, false);
    char lines[3][96]; int n = wrap_text(small, r->text, w - 40, lines, 3);
    int shown = (int)(t * 60);
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(lines[i]);
        font_draw_n(small, lines[i], shown < l ? (shown < 0 ? 0 : shown) : l, x0 + 34, y0 + 3 + i * 9, 220, 235, 255);
        shown -= l + 1;
    }
}

static void render_instructions(Space *s, Font *f, Font *small)
{
    float t = s->phase_t; int sw = s->sw, sh = s->sh;
    float open = clampf(t / INSTR_OPEN, 0, 1); open = 1 - (1 - open) * (1 - open);
    SDL_SetRenderDrawBlendMode(s->ren, SDL_BLENDMODE_BLEND);
    rect(s->ren, 0, 0, (float)sw, (float)sh, 0, 0, 0, (uint8_t)(140 * open));
    static const struct { const char *label, *desc; } LINES[] = {
        { "FLY", "Arrows (hold Aim for fine steering)" }, { "GUNS", "Shoot button - hold it" },
        { "TORPEDO", "Jump button - clears the screen" }, { "SPECIAL", "Power button - hero bomb (x2)" },
        { "PICK UP", "P power  S shield  B torpedo" },
    };
    int nl = (int)(sizeof LINES / sizeof *LINES);
    float full_h = 34 + nl * 14 + 22, pw = (float)sw - 40, ph = full_h * open, x0 = 20, y0 = (sh - full_h) * 0.5f + (full_h - ph) * 0.5f;
    rect(s->ren, x0, y0, pw, ph, 10, 18, 44, (uint8_t)(255 * open));
    rect(s->ren, x0, y0 - 2, pw, 2, 60, 120, 220, (uint8_t)(255 * open)); rect(s->ren, x0, y0 + ph, pw, 2, 60, 120, 220, (uint8_t)(255 * open));
    if (open < 1 || !f || !small) return;
    const char *title = "RAMROD - CRUISER MODE";
    font_draw(f, title, x0 + (pw - font_text_width(f, title)) * 0.5f, y0 + 8, 255, 182, 0);
    for (int i = 0; i < nl; i++) {
        font_draw(small, LINES[i].label, x0 + 14, y0 + 30 + i * 14, 255, 224, 192);
        font_draw(small, LINES[i].desc, x0 + (sw < 400 ? 76 : 96), y0 + 30 + i * 14, 220, 230, 255);
    }
    const char *sub = "CATCH THE OUTRIDER BATTLE CRUISER";
    font_draw(small, sub, x0 + (pw - font_text_width(small, sub)) * 0.5f, y0 + full_h - 26, 255, 255, 255);
    if (t >= INSTR_OPEN && ((int)(t * 4) & 1)) { const char *m = "PRESS A BUTTON"; font_draw(small, m, x0 + (pw - font_text_width(small, m)) * 0.5f, y0 + full_h - 12, 200, 200, 200); }
}

void space_draw(Space *s, bool scanlines)
{
    if (!s->ok) return;
    SDL_Renderer *ren = s->ren;
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    float shx = 0, shy = 0;
    if (s->shake > 0) { float k = fminf(1, s->shake * 3); shx = (frand(s) - 0.5f) * 6 * k; shy = (frand(s) - 0.5f) * 5 * k; }
    SDL_Rect vp = { (int)shx, (int)shy, s->sw, s->sh }; SDL_SetRenderViewport(ren, &vp);
    render_background(s);
    render_boss(s);
    render_fx(s, true);
    render_foes(s);
    render_shots(s);
    render_player(s);
    render_fx(s, false);
    render_beam(s);
    SDL_SetRenderViewport(ren, NULL);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (s->red > 0) rect(ren, 0, 0, (float)s->sw, (float)s->sh, 200, 20, 10, (uint8_t)(90 * clampf(s->red, 0, 1)));
    if (s->phase != PH_INSTR) render_hud(s, small);
    if (f && small) {
        if (s->phase == PH_WARNING) {
            bool on = (int)(s->phase_t * 4) & 1;
            rect(ren, 0, 64, (float)s->sw, 40, 120, 0, 0, 110);
            if (on) center_text(s, f, "WARNING!", 70, 255, 60, 60);
            center_text(s, small, "OUTRIDER BATTLE CRUISER APPROACHING", 90, 255, 220, 200);
        }
        if (s->msg_t > 0 && s->phase != PH_WARNING) { center_text(s, f, s->msg, 70, 255, 255, 255); if (s->msg2[0]) center_text(s, small, s->msg2, 88, 255, 200, 160); }
        if (s->dead && s->dead_t > 0.4f && s->phase != PH_GAMEOVER) center_text(s, f, s->lives > 0 ? "RAMROD HIT!" : "RAMROD IS DOWN!", 100, 255, 60, 60);
        if (s->paused) center_text(s, f, "PAUSE", 110, 255, 255, 255);
    }
    render_radio(s, small);
    if (s->dlg.active) dialog_draw(&s->dlg, ren, s->sw, s->sh);
    if (s->phase == PH_INSTR) render_instructions(s, f, small);
    if (s->white > 0) rect(ren, 0, 0, (float)s->sw, (float)s->sh, 255, 255, 255, (uint8_t)(255 * clampf(s->white, 0, 1)));
    power_draw(&s->pw, ren, s->sw, s->sh);
    if (scanlines) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 70);
        for (int y = 1; y < s->sh; y += 2) { SDL_FRect q = { 0, (float)y, (float)s->sw, 1 }; SDL_RenderFillRect(ren, &q); }
    }
    float fade = s->phase == PH_CLEARED ? clampf(s->phase_t / 1.6f, 0, 1) : s->black;
    if (fade > 0) { uint8_t v = s->phase == PH_CLEARED ? 255 : 0; rect(ren, 0, 0, (float)s->sw, (float)s->sh, v, v, v, (uint8_t)(255 * fade)); }
}
