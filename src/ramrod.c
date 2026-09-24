#include "ramrod.h"
#include "assets.h"
#include "gfx.h"
#include "font.h"
#include "audio.h"
#include "dialog.h"
#include "heroes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI R(3.1415927f)
#define TWO_PI R(6.2831853f)

/* ---- view: the cockpit art is a 426x240 screen; row 167 is the first row of sand in the clip ---- */
#define ART_W 426
#define HZ R(167.0f)            /* screen row of the horizon */
#define FOCAL R(213.9f)         /* px; the panorama is 1344 px = 360 degrees at this focal length */
#define PANO_W 1344
#define PANO_H 170
#define PANO_AHEAD R(513.0f)    /* panorama x straight ahead at the start heading (the planet) */
#define HEADING0 (-PI / 2)   /* north: towards the ridge the Renegades come over */
#define CAM_H R(45.0f)          /* Ramrod's eye height, world units: low enough that a mech's chest stands above the seats */
#define UNIT R(0.85f)           /* world units per sprite pixel at scale 1 (the mech canvas: ~110 units tall) */
#define AIM_Y R(160.0f)         /* the reticle's row */
#define FOG0 R(700.0f)
#define FOG1 R(2600.0f)
#define TEX 256              /* floor texture, 1 texel = 1 unit */
#define MIPS 4

/* ---- world ---- */
#define ARENA_R R(1900.0f)      /* the battle zone around the outpost */
#define PLAYER_R R(60.0f)
#define MECH_R R(34.0f)
#define MECH_H R(115.0f)
#define PUNCH_RANGE R(250.0f)   /* centre to centre */
#define MAX_MECH 12
#define MAX_SHOT 64
#define MAX_FX 160
#define MAX_PROP 48

/* ---- atlas ---- */
enum { A_ARM, A_MECH, A_MECH_RED, A_MECH_GOLD, A_EXPL, A_SMOKE, A_ROCK_BIG, A_ROCK_SMALL, A_MESA, A_CACTUS, A_BOLT,
       A_PLASMA, A_FLASH, A_MFLASH, A_DEBRIS, A_SCORCH, A_COUNT };
static const char *const ANAMES[A_COUNT] = { "arm", "mech", "mech_red", "mech_gold", "expl", "smoke", "rock_big",
    "rock_small", "mesa", "cactus", "bolt", "plasma", "flash", "mflash", "debris", "scorch" };
#define MAX_FRAMES 12
typedef struct { int x, y, w, h, ax, ay; } Frame;
typedef struct { Frame f[MAX_FRAMES]; int n; } Anim;

/* ---- mech frames (ramrod/mech_frames.py) ---- */
enum { MF_WALK0, MF_WALK1, MF_WALK2, MF_WALK3, MF_AIM, MF_WINDUP, MF_PUNCH, MF_STAGGER };
/* ---- mech kinds ---- */
enum { V_GRUNT, V_HEAVY, V_COMMANDER };
/* rest: the least time between volleys (up to twice that); aim: the wind-up before one */
static const struct { int anim; real hp, speed, scale; int volley; real shot_dmg, punch_dmg, rest, aim; const char *name; } VARIANT[3] = {
    { A_MECH,      R(16), R(76), R(1.00f), 1, R(8), R(14), R(1.0f), R(0.70f), "RENEGADE" },
    { A_MECH_RED,  R(34), R(84), R(1.06f), 3, R(6), R(16), R(1.1f), R(0.85f), "RENEGADE HEAVY" },
    { A_MECH_GOLD, R(90), R(78), R(1.30f), 4, R(8), R(22), R(0.5f), R(0.70f), "RENEGADE COMMANDER" },
};
enum { M_OFF, M_ENTER, M_APPROACH, M_CIRCLE, M_AIM, M_FIRE, M_CHARGE, M_WINDUP, M_PUNCH, M_STAGGER, M_DYING };
typedef struct {
    int st, variant; real x, y, hp, hp_max, st_t, anim, strafe, pref, kvx, kvy, flash, scale, fire_t, dying_t, expl_t;
    int volley_left; bool step_sfx;
    real punch_dir;   /* where the swing goes: locked when it winds up, so a sidestep makes it miss */
} Mech;

typedef struct { bool on, enemy; real x, y, z, vx, vy, vz, life, px, py, pz, dmg; int side; } Shot;
enum { FX_EXPL, FX_SMOKE, FX_DEBRIS, FX_FLASH, FX_SCORCH };
typedef struct { bool on; int kind, frame; real x, y, z, vx, vy, vz, t, dur, scale, spin; bool flip; } Fx;
typedef struct { int anim; real x, y, scale, r; bool flip; } Prop;

/* ---- waves ---- */
typedef struct { int variant; real ang, delay; } Spawn;
typedef struct { const char *title, *sub; int n, max_active; Spawn s[9]; } Wave;
static const Wave WAVES[3] = {
    { "WAVE 1", "OUTRIDER VANGUARD", 4, 2, { { V_GRUNT, R(0.0f), R(0.0f) }, { V_GRUNT, R(0.45f), R(2.0f) }, { V_GRUNT, R(-0.5f), R(6.0f) }, { V_GRUNT, R(0.2f), R(10.0f) } } },
    { "WAVE 2", "THE PINCER", 6, 3, { { V_GRUNT, R(-1.3f), R(0.0f) }, { V_GRUNT, R(1.3f), R(0.5f) }, { V_HEAVY, R(0.1f), R(4.0f) },
                                     { V_GRUNT, R(3.0f), R(7.0f) }, { V_HEAVY, R(-2.2f), R(10.0f) }, { V_GRUNT, R(-0.4f), R(12.0f) } } },
    { "WAVE 3", "RENEGADE SQUADRON", 8, 3, { { V_COMMANDER, R(0.0f), R(0.0f) }, { V_GRUNT, R(-0.6f), R(0.3f) }, { V_GRUNT, R(0.6f), R(0.6f) },
                                     { V_HEAVY, R(2.4f), R(6.0f) }, { V_GRUNT, R(-2.4f), R(8.0f) }, { V_HEAVY, R(3.1f), R(13.0f) }, { V_GRUNT, R(1.6f), R(15.0f) },
                                     { V_HEAVY, R(-1.2f), R(18.0f) } } },
};
#define N_WAVES 3

/* ---- the story (series pilot "Star Sheriff Round-Up": Yuma, the Outriders' Renegade battle robots; Ramrod's
 * "Power Stride - and ready to ride!"). After the cave lab the team heads home over Yuma; the outpost calls for
 * help. Ramrod in robot mode is everyone's: Fireball at the controls, Colt on the guns, Saber commanding, April
 * running the systems - the hero picked for the run doesn't change the scenes. ---- */
static const char *const SCRIPT_INTRO =
    "<|RED|>\n</dialog_avatar_april2/>\nMayday from the Yuma frontier outpost! Outrider battle mechs - a whole Renegade squadron marching on the town!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThose settlers have nothing that can stop a Renegade. We do. Everyone to your stations!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nBig iron against big iron. Now THAT'S my kind of showdown.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nThen let's do it right. Ramrod... POWER STRIDE!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_april2/>\n...AND READY TO RIDE!\n<<>>\n";
static const char *const SCRIPT_AFTER1 =
    "<|GREEN|>\n</dialog_avatar_colt2/>\nScratch three tin cans! Keep 'em coming, I'm just warming up.\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nDon't get cocky, Colt - contacts on every side. They're trying to box us in! Watch the radar!\n<<>>\n";
static const char *const SCRIPT_AFTER2 =
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThat red one fought like a veteran. Fireball, how is Ramrod holding up?\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nDented, not beaten. April, patch what you can.\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nRerouting power... Oh no. A command mech is leading the next group - and it's twice their size!\n<<>>\n";
/* the bridge into the final phase (space.c): the Renegades were dropped from orbit, and the ship that dropped them is
 * running for a dimension jump to fetch the rest of the fleet */
static const char *const SCRIPT_OUTRO =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nEnjoy it, Star Sheriffs... we were only the vanguard. Our battle cruiser is already breaking orbit - and it's coming back with the whole fleet!\n<<>>\n"
    "<|RED|>\n</dialog_avatar_april2/>\nHe's not bluffing! Long-range scan: an Outrider battle cruiser, climbing out of Yuma's orbit. It's heading for a dimension jump point!\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nIf it makes that jump, it brings Nemesis a fleet. We stop it before it gets there - in space.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_colt2/>\nChasin' a battleship through a minefield? Partner, I thought you'd never ask.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nEveryone strap in. Ramrod - back to CRUISER MODE! Full thrust!\n<<>>\n";
static const char *const *const SCRIPT_BETWEEN[N_WAVES] = { &SCRIPT_AFTER1, &SCRIPT_AFTER2, NULL };

enum { PH_INTRO, PH_STRIDE, PH_INSTR, PH_WAVE_IN, PH_FIGHT, PH_WAVE_CLEAR, PH_RADIO, PH_DOWN, PH_GAMEOVER, PH_OUTRO, PH_CLEARED };
#define STRIDE_DUR R(2.2f)
#define INSTR_OPEN R(0.5f)
#define INSTR_MIN R(2.4f)
#define WAVE_IN_DUR R(2.6f)

struct Ramrod {
    Ren *ren; int sw, sh; bool ok;
    RTex *atlas, *sky, *cockpit; RFloor *floor; Anim anim[A_COUNT];
    uint32_t tex[MIPS][TEX * TEX]; int floor_h;
    int difficulty, lives, result;
    /* Ramrod */
    real px, py, heading, turn_v, speed, strafe_v, armor, armor_max, heat, fire_cd, gun_idle; bool overheated; int gun_side;
    real step_phase, bob, shake, hurt_t, alarm_t; bool fire_hold, punch_hold;
    real punch_t; int punch_side; bool punch_hit_done; real punch_cd;
    int lock; real lock_t;
    /* world */
    Mech mech[MAX_MECH]; Shot shot[MAX_SHOT]; Fx fx[MAX_FX]; Prop prop[MAX_PROP]; int nprop;
    /* flow */
    int phase, wave, spawned, kills; real phase_t, wave_t, total_t;
    Dialog dlg; bool paused, dlg_pending; const char *pending_script;
    char msg[48], msg2[48]; real msg_t;
    real white, red, black;
    unsigned rng; int music_now;
    bool god;
};

/* ---------------------------------------------------------------- helpers */
#ifdef REAL_FIXED
static real frand(Ramrod *r) { r->rng = r->rng * 1664525u + 1013904223u; return (real)(r->rng >> 16); }   /* 16 bits of fraction */
#else
static float frand(Ramrod *r) { r->rng = r->rng * 1664525u + 1013904223u; return (r->rng >> 8) / 16777216.0f; }
#endif
static real clampf(real v, real lo, real hi) { return v < lo ? lo : v > hi ? hi : v; }
static real approach(real v, real t, real d) { return v < t ? r_min(v + d, t) : r_max(v - d, t); }
static real xoff(const Ramrod *r) { return r_int(r->sw - ART_W) / 2; }   /* * 0.5f */
static void set_msg(Ramrod *r, const char *a, const char *b, real t) { snprintf(r->msg, sizeof r->msg, "%s", a); snprintf(r->msg2, sizeof r->msg2, "%s", b ? b : ""); r->msg_t = t; }
static void play_music(Ramrod *r, int track) { if (r->music_now != track) { music_play(track, true); r->music_now = track; music_set_volume(R(1.0f)); } }
static void sfx_file(const char *name) { char buf[64]; snprintf(buf, sizeof buf, "ramrod/%s", name); sfx_play_file(asset_path(buf)); }
static real dmg_mul(const Ramrod *r) { return r->difficulty == 0 ? R(0.6f) : r->difficulty == 1 ? R(1.0f) : R(1.35f); }

/* camera space: f = distance ahead, l = to the right */
static void to_cam(const Ramrod *r, real x, real y, real *f, real *l)
{
    real dx = x - r->px, dy = y - r->py, c = r_cos(r->heading), s = r_sin(r->heading);
    *f = r_mul(dx, c) + r_mul(dy, s); *l = r_mul(-dx, s) + r_mul(dy, c);
}
static real horizon(const Ramrod *r) { return HZ + r->bob; }
static bool project(const Ramrod *r, real x, real y, real z, real *sx, real *sy, real *k)
{
    real f, l; to_cam(r, x, y, &f, &l);
    if (f < R(8)) return false;
    *sx = r_int(r->sw) / 2 + r_muldiv(l, FOCAL, f); *sy = horizon(r) + r_muldiv(CAM_H - z, FOCAL, f); if (k) *k = r_div(FOCAL, f);
    return true;
}

static Fx *fx_new(Ramrod *r) { for (int i = 0; i < MAX_FX; i++) if (!r->fx[i].on) { Fx *e = &r->fx[i]; memset(e, 0, sizeof *e); e->on = true; e->scale = R(1); return e; } return NULL; }
static void spawn_expl(Ramrod *r, real x, real y, real z, real scale)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_EXPL; e->x = x; e->y = y; e->z = z; e->scale = scale; e->dur = R(0.5f); e->flip = frand(r) < R(0.5f);
}
static void spawn_flash(Ramrod *r, real x, real y, real z, real scale, bool enemy)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_FLASH; e->x = x; e->y = y; e->z = z; e->scale = scale; e->dur = R(0.09f); e->frame = enemy ? 1 : 0;
}
static void spawn_smoke(Ramrod *r, real x, real y, real z, real scale)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_SMOKE; e->x = x; e->y = y; e->z = z; e->vz = R(30) + frand(r) * 20; e->scale = scale; e->dur = R(1.1f);
}

/* ---------------------------------------------------------------- loading */
static RTex *load_tex(Ramrod *r, const char *name, int *w, int *h, uint32_t **keep)
{
    char buf[64]; snprintf(buf, sizeof buf, "ramrod/%s", name);
    const char *p = asset_path(buf);
    if (!p) { fprintf(stderr, "assets/%s missing (run ../ramrod/build.py)\n", buf); return NULL; }
    int ww, hh; RTex *t = gfx_image_tex(p, &ww, &hh);   /* the console's baked texture, else the PNG */
    if (!t) return NULL;
    rtex_set_blend(t, R_BLEND_BLEND); rtex_set_scale(t, R_SCALE_NEAREST);
    if (w) *w = ww;
    if (h) *h = hh;
    if (keep && !(*keep = png_load_rgba(p, &ww, &hh))) { rtex_destroy(t); return NULL; }   /* the pixels the code reads */
    return t;
}

static bool load_assets(Ramrod *r)
{
    static const char *const SFX[] = { "alarm.wav", "charge.wav", "clang.wav", "laser.wav", "stomp.wav", "whoosh.wav" };   /* its sounds, loaded before they play */
    for (size_t i = 0; i < sizeof SFX / sizeof *SFX; i++) { char b[64]; snprintf(b, sizeof b, "ramrod/%s", SFX[i]); sfx_preload_file(asset_path(b)); }
    int w, h;
    r->atlas = load_tex(r, "atlas.png", NULL, NULL, NULL);
    r->sky = load_tex(r, "sky.png", &w, &h, NULL);
    r->cockpit = load_tex(r, "cockpit.png", NULL, NULL, NULL);
    const char *fp = asset_path("ramrod/floor.png");
    uint32_t *fl = fp ? png_load_rgba(fp, &w, &h) : NULL;   /* the floor is only read as pixels (r_floor_create) */
    if (!r->atlas || !r->sky || !r->cockpit || !fl || w != TEX || h != TEX) { free(fl); return false; }
    memcpy(r->tex[0], fl, sizeof r->tex[0]); free(fl);
    for (int L = 1; L < MIPS; L++) {   /* box-filtered mips against far-row shimmer */
        int n = TEX >> L, pn = TEX >> (L - 1); const uint32_t *src = r->tex[L - 1]; uint32_t *dst = r->tex[L];
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            uint32_t c[4] = { src[(2 * y) * pn + 2 * x], src[(2 * y) * pn + 2 * x + 1], src[(2 * y + 1) * pn + 2 * x], src[(2 * y + 1) * pn + 2 * x + 1] };
            uint32_t R = 0, G = 0, B = 0;
            for (int k = 0; k < 4; k++) { R += c[k] & 0xff; G += (c[k] >> 8) & 0xff; B += (c[k] >> 16) & 0xff; }
            dst[y * n + x] = 0xff000000u | ((B + 2) / 4) << 16 | ((G + 2) / 4) << 8 | ((R + 2) / 4);
        }
    }
    const char *p = asset_path("ramrod/atlas.txt");
    FILE *f = asset_fopen(p);
    if (!f) return false;
    char name[64]; int fr, x, y, fw, fh, ax, ay;
    while (fscanf(f, "%63s %d %d %d %d %d %d %d", name, &fr, &x, &y, &fw, &fh, &ax, &ay) == 8)
        for (int i = 0; i < A_COUNT; i++) if (!strcmp(name, ANAMES[i]) && fr < MAX_FRAMES) {
            r->anim[i].f[fr] = (Frame){ x, y, fw, fh, ax, ay }; if (fr + 1 > r->anim[i].n) r->anim[i].n = fr + 1;
        }
    fclose(f);
    for (int i = 0; i < A_COUNT; i++) if (!r->anim[i].n) { fprintf(stderr, "ramrod atlas: %s missing\n", ANAMES[i]); return false; }
    return true;
}

/* ---------------------------------------------------------------- setup */
static void place_props(Ramrod *r)
{
    /* boulders, buttes and the odd cactus around the outpost's approaches; nothing in the middle where it starts */
    r->nprop = 0;
    for (int i = 0; i < 40 && r->nprop < MAX_PROP; i++) {
        real a = r_mul(frand(r), TWO_PI), d = R(450) + r_mul(frand(r), ARENA_R + R(600) - R(450));
        Prop *p = &r->prop[r->nprop++];
        real u = frand(r);
        p->anim = u < R(0.35f) ? A_ROCK_BIG : u < R(0.65f) ? A_ROCK_SMALL : u < R(0.8f) ? A_CACTUS : A_MESA;
        p->scale = p->anim == A_MESA ? R(2.2f) + frand(r) : p->anim == A_ROCK_BIG ? R(1.1f) + r_mul(frand(r), R(0.6f)) : p->anim == A_CACTUS ? R(0.7f) : R(1.0f) + r_mul(frand(r), R(0.5f));
        if (p->anim == A_MESA) d = ARENA_R + R(300) + frand(r) * 600;   /* the buttes stand at the zone's edge */
        p->x = r_mul(r_cos(a), d); p->y = r_mul(r_sin(a), d); p->flip = frand(r) < R(0.5f);
        p->r = (p->anim == A_ROCK_BIG ? 26 : p->anim == A_ROCK_SMALL ? 12 : p->anim == A_MESA ? 50 : 6) * p->scale;
    }
}

static void clear_world(Ramrod *r)
{
    memset(r->mech, 0, sizeof r->mech); memset(r->shot, 0, sizeof r->shot);
    for (int i = 0; i < MAX_FX; i++) if (r->fx[i].kind != FX_SCORCH) r->fx[i].on = false;
    r->lock = -1;
}

static void begin_wave(Ramrod *r, int w)
{
    clear_world(r);
    r->wave = w; r->spawned = 0; r->wave_t = 0;
    r->phase = PH_WAVE_IN; r->phase_t = 0;
    play_music(r, w == N_WAVES - 1 ? 8 : 16);
    sfx_file("alarm.wav");
}

Ramrod *ramrod_create(Ren *ren, int sw, int sh, int difficulty, int lives)
{
    Ramrod *r = calloc(1, sizeof *r);
    r->ren = ren; r->sw = sw; r->sh = sh; r->rng = 0x5AB3E7u; r->music_now = -1;
    r->floor_h = sh - r_trunc(HZ) + 8;
    r->ok = load_assets(r);
    r->difficulty = difficulty; r->lives = lives;
    r->armor = r->armor_max = R(100); r->heading = HEADING0; r->lock = -1; r->punch_t = R(-1);
    r->god = plat_getenv("SABER_R6GOD") != NULL;
    dialog_set_hero(HERO_FIREBALL);   /* Ramrod is everyone's: the scenes are written for the whole crew */
    place_props(r);
    r->phase = PH_INTRO; r->dlg_pending = true; r->pending_script = SCRIPT_INTRO;
    play_music(r, 16);
    if (plat_getenv("SABER_R6WAVE")) {   /* debug: straight into wave n (1..3), no story */
        int w = atoi(plat_getenv("SABER_R6WAVE")) - 1; r->dlg_pending = false;
        begin_wave(r, w < 0 ? 0 : w >= N_WAVES ? N_WAVES - 1 : w);
    }
    return r;
}

void ramrod_destroy(Ramrod *r)
{
    if (!r) return;
    if (r->atlas) rtex_destroy(r->atlas);
    if (r->sky) rtex_destroy(r->sky);
    if (r->cockpit) rtex_destroy(r->cockpit);
    if (r->floor) r_floor_destroy(r->floor);
    free(r);
}
int ramrod_result(const Ramrod *r) { return r->result; }
int ramrod_lives(const Ramrod *r) { return r->lives; }

/* ---------------------------------------------------------------- combat */
static bool fighting(const Ramrod *r) { return r->phase == PH_FIGHT || r->phase == PH_WAVE_IN; }

static void player_hurt(Ramrod *r, real dmg, real shake, const char *what)
{
    if (!fighting(r) || r->god) return;
    if (plat_getenv("SABER_TRACE")) fprintf(stderr, "r6 hurt %s %s (armor %s)\n", what, RS(r_mul(dmg, dmg_mul(r)), 0), RS(r->armor, 0));
    r->armor -= r_mul(dmg, dmg_mul(r)); r->hurt_t = R(0.5f); r->shake = r_max(r->shake, shake); r->red = r_max(r->red, R(0.55f));
    sfx_play(3, 0);
    if (r->armor <= 0) {
        r->armor = 0; r->phase = PH_DOWN; r->phase_t = 0; sfx_play(5, 0); sfx_play(6, 4); sfx_play(0x15, 10);
        music_stop(); r->music_now = -1;
    }
}

static void mech_kill(Ramrod *r, Mech *m)
{
    m->st = M_DYING; m->dying_t = 0; m->expl_t = 0; m->hp = 0;
    sfx_play(0x11, 0); r->kills++;
    if (r->lock >= 0 && &r->mech[r->lock] == m) r->lock = -1;
}

static void mech_damage(Ramrod *r, Mech *m, real dmg, bool punch)
{
    if (m->st == M_DYING || m->st == M_OFF) return;
    m->hp -= dmg; m->flash = R(0.12f);
    if (m->hp <= 0) { mech_kill(r, m); return; }
    if (punch) {   /* a fist rocks it back; one caught winding up loses its punch */
        real c = r_cos(r->heading), s = r_sin(r->heading);
        bool was_windup = m->st == M_WINDUP;
        m->kvx = c * 300; m->kvy = s * 300; m->st = M_STAGGER; m->st_t = was_windup ? R(1.0f) : R(0.75f);
    }
}

static void fire_bolt(Ramrod *r)
{
    Shot *s = NULL; for (int i = 0; i < MAX_SHOT; i++) if (!r->shot[i].on) { s = &r->shot[i]; break; }
    if (!s) return;
    memset(s, 0, sizeof *s);
    real c = r_cos(r->heading), sn = r_sin(r->heading), side = r->gun_side ? R(1.0f) : R(-1.0f);
    r->gun_side ^= 1;
    /* Ramrod's shoulder guns: below and to either side of the view, converging on the reticle */
    s->x = r->px + c * 50 - r_mul(sn, side) * 44; s->y = r->py + sn * 50 + r_mul(c, side) * 44; s->z = CAM_H - R(30);
    real tx, ty, tz;
    if (r->lock >= 0) { Mech *m = &r->mech[r->lock]; tx = m->x; ty = m->y; tz = 70 * m->scale; }
    else { tx = r->px + c * 900; ty = r->py + sn * 900; tz = CAM_H - r_div((HZ - AIM_Y) * 900, FOCAL); }
    real dx = tx - s->x, dy = ty - s->y, dz = tz - s->z, d = r_len3(dx, dy, dz);
    const real V = R(1500);
    s->vx = r_mul(r_div(dx, d), V); s->vy = r_mul(r_div(dy, d), V); s->vz = r_mul(r_div(dz, d), V); s->life = R(1.2f); s->on = true; s->side = side > 0;
    s->px = s->x; s->py = s->y; s->pz = s->z;
    sfx_file("laser.wav");
}

static void fire_plasma(Ramrod *r, Mech *m)
{
    Shot *s = NULL; for (int i = 0; i < MAX_SHOT; i++) if (!r->shot[i].on) { s = &r->shot[i]; break; }
    if (!s) return;
    memset(s, 0, sizeof *s);
    /* from the cannon fist: right of the mech as the player sees it, at chest height */
    real dx = r->px - m->x, dy = r->py - m->y, d = r_hypot(dx, dy); if (d < R(1)) d = R(1);
    real rx = r_div(-dy, d), ry = r_div(dx, d);   /* the viewer's right, seen from the mech: its left... */
    s->x = m->x - r_mul(rx * 22, m->scale); s->y = m->y - r_mul(ry * 22, m->scale); s->z = 78 * m->scale;
    if (m->variant != V_GRUNT) {   /* the veterans lead a moving target */
        real c = r_cos(r->heading), sn = r_sin(r->heading), t = r_div(d, R(410.0f));
        dx += r_mul(r_mul(r_mul(c, r->speed) - r_mul(sn, r->strafe_v), t), R(0.8f)); dy += r_mul(r_mul(r_mul(sn, r->speed) + r_mul(c, r->strafe_v), t), R(0.8f));
    }
    real spread = m->volley_left > 0 && VARIANT[m->variant].volley > 1 ? ((m->volley_left % 3) - 1) * R(0.09f) : 0;
    real ang = r_atan2(dy, dx) + spread + r_mul(frand(r) - R(0.5f), R(0.04f));
    const real V = r->difficulty == 2 ? R(470) : r->difficulty == 1 ? R(410) : R(350);
    s->vx = r_mul(r_cos(ang), V); s->vy = r_mul(r_sin(ang), V); s->vz = r_div((CAM_H - R(8)) - s->z, r_div(d, V));
    s->life = r_div(d, V) + R(1.5f); s->on = true; s->enemy = true; s->dmg = VARIANT[m->variant].shot_dmg; s->px = s->x; s->py = s->y; s->pz = s->z;
    spawn_flash(r, s->x, s->y, s->z, r_mul(R(1.2f), m->scale), true);
    sfx_play(0x12, 0);
}

static int attackers(const Ramrod *r, bool melee_only)
{
    int n = 0;
    for (int i = 0; i < MAX_MECH; i++) {
        int st = r->mech[i].st;
        if (melee_only ? (st == M_CHARGE || st == M_WINDUP || st == M_PUNCH) : (st == M_AIM || st == M_FIRE || st == M_CHARGE || st == M_WINDUP || st == M_PUNCH)) n++;
    }
    return n;
}

static void spawn_mech(Ramrod *r, const Spawn *sp)
{
    Mech *m = NULL; for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st == M_OFF) { m = &r->mech[i]; break; }
    if (!m) return;
    memset(m, 0, sizeof *m);
    const real hpmul = r->difficulty == 0 ? R(0.8f) : r->difficulty == 1 ? R(1.0f) : R(1.25f);
    real a = r->heading + sp->ang, d = R(1100) + frand(r) * 250;
    m->variant = sp->variant; m->x = r->px + r_mul(r_cos(a), d); m->y = r->py + r_mul(r_sin(a), d);
    m->hp = m->hp_max = r_mul(VARIANT[sp->variant].hp, hpmul); m->scale = VARIANT[sp->variant].scale;
    m->st = M_ENTER; m->strafe = frand(r) < R(0.5f) ? R(-1) : R(1); m->pref = R(380) + frand(r) * 320; m->anim = frand(r) * 4;
    m->fire_t = R(0.5f) + r_mul(frand(r), R(1.0f));
}

static void push_apart(real *ax, real *ay, real bx, real by, real minr)
{
    real dx = *ax - bx, dy = *ay - by, d = r_hypot(dx, dy);
    if (d < minr && d > R(0.01f)) { *ax = bx + r_mul(r_div(dx, d), minr); *ay = by + r_mul(r_div(dy, d), minr); }
}

static void mech_update(Ramrod *r, Mech *m, int idx, real dt)
{
    const real diffk = r->difficulty == 0 ? R(0.75f) : r->difficulty == 1 ? R(1.0f) : R(1.25f);
    real dx = r->px - m->x, dy = r->py - m->y, d = r_hypot(dx, dy); if (d < R(1)) d = R(1);
    real ux = r_div(dx, d), uy = r_div(dy, d), mvx = 0, mvy = 0, spd = VARIANT[m->variant].speed;
    if (m->flash > 0) m->flash -= dt;
    m->st_t -= dt;
    switch (m->st) {
    case M_ENTER:      /* marching in from the ridge */
        mvx = r_mul(ux, spd); mvy = r_mul(uy, spd);
        if (d < R(950)) { m->st = M_APPROACH; }
        break;
    case M_APPROACH:
        mvx = r_mul(ux, spd); mvy = r_mul(uy, spd);
        m->fire_t -= r_mul(dt, diffk);
        if (d < m->pref) { m->st = M_CIRCLE; m->st_t = R(1.0f) + r_mul(frand(r), R(1.5f)); }
        else if (m->fire_t <= 0 && attackers(r, false) < 2 + r->difficulty + (m->variant == V_COMMANDER)) { m->st = M_AIM; m->st_t = r_div(VARIANT[m->variant].aim, diffk); }   /* opens fire on the way in */
        break;
    case M_CIRCLE: {   /* strafe round the player, holding the preferred range */
        real rad = r_mul(d - m->pref, R(0.8f));
        mvx = r_mul(r_mul(r_mul(-uy, m->strafe), spd), R(0.8f)) + r_mul(ux, clampf(rad, -spd, spd));
        mvy = r_mul(r_mul(r_mul(ux, m->strafe), spd), R(0.8f)) + r_mul(uy, clampf(rad, -spd, spd));
        m->fire_t -= r_mul(dt, diffk);
        if (d < R(230) && attackers(r, true) == 0) { m->st = M_WINDUP; m->st_t = r_div(R(0.65f), diffk); m->punch_dir = r_atan2(dy, dx); sfx_file("charge.wav"); break; }   /* too close: it swings */
        if (m->st_t <= 0) {
            m->st_t = R(0.6f) + r_mul(frand(r), R(1.0f));
            if (frand(r) < R(0.3f)) m->strafe = -m->strafe;
            int cap = 2 + r->difficulty + (m->variant == V_COMMANDER);   /* the command mech doesn't wait its turn */
            if (d < R(700) && frand(r) < r_mul(R(0.45f), diffk) && attackers(r, true) == 0 && attackers(r, false) < cap) { m->st = M_CHARGE; m->st_t = R(3.0f); }
            else if (m->fire_t <= 0 && d < R(1700) && attackers(r, false) < cap) { m->st = M_AIM; m->st_t = r_div(VARIANT[m->variant].aim, diffk); if (d < R(1000)) sfx_file("charge.wav"); }
            else m->pref = R(360) + frand(r) * 360;
        }
        break; }
    case M_AIM:
        if (m->st_t <= 0) { m->st = M_FIRE; m->volley_left = VARIANT[m->variant].volley; m->st_t = 0; }
        break;
    case M_FIRE:
        if (m->st_t <= 0) {
            fire_plasma(r, m); m->volley_left--; m->st_t = R(0.24f);
            if (m->volley_left <= 0) { m->st = M_CIRCLE; m->st_t = R(0.4f) + r_mul(frand(r), R(0.5f)); m->fire_t = r_mul(VARIANT[m->variant].rest, R(1) + frand(r)); }
        }
        break;
    case M_CHARGE:     /* runs at the player */
        mvx = r_mul(r_mul(ux, spd), R(2.1f)); mvy = r_mul(r_mul(uy, spd), R(2.1f));
        if (d < R(225)) { m->st = M_WINDUP; m->st_t = r_div(R(0.6f), diffk); m->punch_dir = r_atan2(dy, dx); sfx_file("charge.wav"); }
        else if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = R(1.0f); }
        break;
    case M_WINDUP:
        if (d > R(260)) { mvx = r_mul(ux, spd); mvy = r_mul(uy, spd); }
        if (m->st_t <= 0) {
            m->st = M_PUNCH; m->st_t = R(0.35f);
            sfx_file("whoosh.wav");
            real off = r_abs(r_remainder(r_atan2(dy, dx) - m->punch_dir, TWO_PI));
            if (d < r_mul(PUNCH_RANGE, m->scale) + R(30) && off < R(0.3f)) { player_hurt(r, VARIANT[m->variant].punch_dmg, R(0.6f), "punch"); sfx_file("clang.wav"); r->white = R(0.25f); }
        }
        break;
    case M_PUNCH:
        if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = R(1.2f); m->pref = R(420) + frand(r) * 200; m->fire_t = r_max(m->fire_t, R(1.0f)); }
        break;
    case M_STAGGER:
        if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = R(0.6f); m->pref = R(380) + frand(r) * 200; }
        break;
    case M_DYING: {   /* bursts all over it while it sinks, then the big one */
        m->dying_t += dt; m->expl_t -= dt;
        if (m->expl_t <= 0 && m->dying_t < R(1.3f)) {
            m->expl_t = R(0.11f);
            real rx = -uy, ry = ux, o = r_mul((frand(r) - R(0.5f)) * 60, m->scale);
            spawn_expl(r, m->x + r_mul(rx, o) - ux * 4, m->y + r_mul(ry, o) - uy * 4, r_mul(R(20) + frand(r) * 90, m->scale), R(0.8f) + r_mul(frand(r), R(0.6f)));
            if (frand(r) < R(0.5f)) sfx_play(5, 0);
        }
        if (m->dying_t >= R(1.5f)) {
            for (int k = 0; k < 3; k++) spawn_expl(r, m->x + (frand(r) - R(0.5f)) * 50, m->y + (frand(r) - R(0.5f)) * 50, r_int(30 + k * 30), R(2.2f) + frand(r));
            for (int k = 0; k < 10; k++) {
                Fx *e = fx_new(r); if (!e) break;
                e->kind = FX_DEBRIS; e->x = m->x; e->y = m->y; e->z = 50 * m->scale; e->frame = k % 6;
                real a = r_mul(frand(r), TWO_PI), v = R(80) + frand(r) * 160;
                e->vx = r_mul(r_cos(a), v); e->vy = r_mul(r_sin(a), v); e->vz = R(180) + frand(r) * 200; e->dur = R(3.0f); e->scale = R(1.2f) + frand(r); e->spin = frand(r) < R(0.5f);
            }
            Fx *sc = fx_new(r); if (sc) { sc->kind = FX_SCORCH; sc->x = m->x; sc->y = m->y; sc->scale = r_mul(R(1.6f), m->scale); sc->dur = R_MAX; }
            for (int k = 0; k < 5; k++) spawn_smoke(r, m->x + (frand(r) - R(0.5f)) * 60, m->y + (frand(r) - R(0.5f)) * 60, R(20), R(1.5f));
            sfx_play(6, 0); r->shake = r_max(r->shake, d < R(600) ? R(0.35f) : R(0.15f));
            m->st = M_OFF;
        }
        return; }
    default: return;
    }
    /* knockback, movement, walking animation */
    m->kvx = approach(m->kvx, 0, r_mul_dt(R(700), dt)); m->kvy = approach(m->kvy, 0, r_mul_dt(R(700), dt));
    real ox = m->x, oy = m->y;
    m->x += r_mul_dt(mvx + m->kvx, dt); m->y += r_mul_dt(mvy + m->kvy, dt);
    /* keep out of Ramrod, the other mechs and the rocks */
    push_apart(&m->x, &m->y, r->px, r->py, PLAYER_R + r_mul(MECH_R, m->scale) + R(60));
    for (int j = 0; j < MAX_MECH; j++) if (j != idx && r->mech[j].st != M_OFF && r->mech[j].st != M_DYING) push_apart(&m->x, &m->y, r->mech[j].x, r->mech[j].y, R(110));
    for (int j = 0; j < r->nprop; j++) push_apart(&m->x, &m->y, r->prop[j].x, r->prop[j].y, r->prop[j].r + MECH_R);
    real moved = r_hypot(m->x - ox, m->y - oy);
    int before = r_trunc(m->anim);
    m->anim += moved / 24;   /* / 24.0f */
    int after = r_trunc(m->anim);
    if (after != before && (after & 1) == 0 && d < R(520)) sfx_file("stomp.wav");   /* its footfalls when it's close */
}

/* ---------------------------------------------------------------- Ramrod */
static void player_control(Ramrod *r, const Input *in, real dt)
{
    bool strafe = btn_down(in, BTN_AIM);
    real turn = 0;
    if (!strafe) { if (btn_down(in, BTN_LEFT)) turn -= R(1); if (btn_down(in, BTN_RIGHT)) turn += R(1); }
    r->turn_v = approach(r->turn_v, r_mul(turn, R(1.7f)), r_mul_dt(R(7.0f), dt));
    r->heading += r_mul_dt(r->turn_v, dt);
    real fwd = 0; if (btn_down(in, BTN_UP)) fwd += R(1); if (btn_down(in, BTN_DOWN)) fwd -= R(1);
    r->speed = approach(r->speed, fwd > 0 ? R(135) : fwd < 0 ? R(-90) : 0, r_mul_dt(R(320), dt));
    real st = 0; if (strafe) { if (btn_down(in, BTN_LEFT)) st -= R(1); if (btn_down(in, BTN_RIGHT)) st += R(1); }
    r->strafe_v = approach(r->strafe_v, st * 120, r_mul_dt(R(420), dt));
    real c = r_cos(r->heading), s = r_sin(r->heading);
    r->px += r_mul_dt(r_mul(c, r->speed) - r_mul(s, r->strafe_v), dt); r->py += r_mul_dt(r_mul(s, r->speed) + r_mul(c, r->strafe_v), dt);
    for (int j = 0; j < r->nprop; j++) push_apart(&r->px, &r->py, r->prop[j].x, r->prop[j].y, r->prop[j].r + PLAYER_R);
    real dc = r_hypot(r->px, r->py);
    if (dc > ARENA_R) {   /* the zone's edge: Ramrod won't leave the outpost behind */
        r->px = r_mul(r->px, r_div(ARENA_R, dc)); r->py = r_mul(r->py, r_div(ARENA_R, dc));
        if (r->msg_t <= 0) set_msg(r, "RETURN TO THE OUTPOST", "THE TOWN IS BEHIND YOU", R(1.5f));
    }
    /* the stride: the view dips on every footfall */
    real gait = r_abs(r->speed) + r_mul(r_abs(r->strafe_v), R(0.8f));
    if (gait > R(5)) {
        real prev = r->step_phase; r->step_phase += r_mul_dt(gait, dt) / 95;   /* / 95.0f */
        if (r_trunc(prev * 2) != r_trunc(r->step_phase * 2)) { sfx_file("stomp.wav"); r->shake = r_max(r->shake, R(0.06f)); }
#ifdef REAL_FIXED
        if (r->step_phase >= R(1000)) r->step_phase -= R(1000);   /* whole strides: the same phase, and x 2 pi stays in range */
#endif
    }
    real target_bob = gait > R(5) ? r_mul(-r_abs(r_sin(r_mul(r->step_phase, TWO_PI))), R(3.0f)) + R(1.5f) : 0;
    r->bob = approach(r->bob, target_bob, r_mul_dt(R(30), dt));

    /* the guns: held fire, alternating shoulders; heat builds up */
    if (r->dlg.active) r->fire_hold = true; else if (!btn_down(in, BTN_SHOOT)) r->fire_hold = false;
    if (r->dlg.active) r->punch_hold = true; else if (!btn_down(in, BTN_JUMP)) r->punch_hold = false;
    r->fire_cd -= dt; r->gun_idle += dt;
    if (btn_down(in, BTN_SHOOT) && !r->fire_hold && !r->overheated && r->fire_cd <= 0) {
        fire_bolt(r); r->fire_cd = R(0.13f); r->heat += R(0.062f); r->gun_idle = 0;
        if (r->heat >= R(1)) { r->heat = R(1); r->overheated = true; set_msg(r, "GUNS OVERHEATED", NULL, R(1.2f)); sfx_file("alarm.wav"); }
    }
    r->heat = r_max(0, r->heat - r_mul(dt, r->gun_idle > R(0.35f) ? R(0.55f) : R(0.22f)));
    if (r->overheated && r->heat < R(0.3f)) r->overheated = false;

    /* the fists: alternate left / right, the hit lands as the arm reaches out */
    r->punch_cd -= dt;
    if (btn_pressed(in, BTN_JUMP) && !r->punch_hold && r->punch_cd <= 0) {
        r->punch_t = 0; r->punch_side ^= 1; r->punch_hit_done = false; r->punch_cd = R(0.5f);
        r->speed += R(50); sfx_file("whoosh.wav");
    }
    if (r->punch_t >= 0) {
        r->punch_t += dt;
        if (!r->punch_hit_done && r->punch_t >= R(0.19f)) {
            r->punch_hit_done = true;
            Mech *best = NULL; real bf = R_MAX;
            for (int i = 0; i < MAX_MECH; i++) {
                Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
                real f, l; to_cam(r, m->x, m->y, &f, &l);
                if (f > 0 && f < PUNCH_RANGE + r_mul(MECH_R, m->scale) && r_abs(l) < R(70) + r_mul(f, R(0.25f)) && f < bf) { bf = f; best = m; }
            }
            if (best) {
                bool counter = best->st == M_WINDUP;
                mech_damage(r, best, counter ? R(9) : R(6), true);
                if (counter && best->st == M_STAGGER) set_msg(r, "COUNTER!", NULL, R(0.8f));
                sfx_file("clang.wav"); sfx_play(14, 0); r->shake = r_max(r->shake, R(0.2f));
                real c2 = r_cos(r->heading), s2 = r_sin(r->heading);
                spawn_flash(r, best->x - c2 * 30, best->y - s2 * 30, 70 * best->scale, R(2.0f), false);
                spawn_expl(r, best->x - c2 * 30, best->y - s2 * 30, 70 * best->scale, R(0.6f));
            }
            for (int i = 0; i < MAX_SHOT; i++) {   /* a fist swats an incoming plasma ball out of the air */
                Shot *s = &r->shot[i]; if (!s->on || !s->enemy) continue;
                real f, l; to_cam(r, s->x, s->y, &f, &l);
                if (f > 0 && f < R(200) && r_abs(l) < R(80)) { s->on = false; spawn_expl(r, s->x, s->y, s->z, R(0.7f)); set_msg(r, "PARRY!", NULL, R(0.7f)); }
            }
        }
        if (r->punch_t > R(0.56f)) r->punch_t = R(-1);
    }
}

static void update_lock(Ramrod *r, real dt)
{
    int best = -1; real ba = R(0.13f);
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
        real f, l; to_cam(r, m->x, m->y, &f, &l);
        if (f < R(40) || f > R(1600)) continue;
        real a = r_abs(r_atan2(l, f)); if (a < ba) { ba = a; best = i; }
    }
    if (best != r->lock) { r->lock = best; r->lock_t = 0; if (best >= 0) sfx_play(0, 0); }
    r->lock_t += dt;
}

static void update_shots(Ramrod *r, real dt)
{
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *s = &r->shot[i]; if (!s->on) continue;
        s->px = s->x; s->py = s->y; s->pz = s->z;
        s->x += r_mul_dt(s->vx, dt); s->y += r_mul_dt(s->vy, dt); s->z += r_mul_dt(s->vz, dt); s->life -= dt;
        if (s->life <= 0 || s->z < 0) { if (s->z < 0) spawn_expl(r, s->x, s->y, R(4), R(0.4f)); s->on = false; continue; }
        if (!s->enemy) {
            for (int j = 0; j < MAX_MECH && s->on; j++) {
                Mech *m = &r->mech[j]; if (m->st == M_OFF || m->st == M_DYING) continue;
                if (r_hypot(s->x - m->x, s->y - m->y) < r_mul(MECH_R, m->scale) + R(6) && s->z < r_mul(MECH_H, m->scale)) {
                    s->on = false; mech_damage(r, m, R(1), false); sfx_play(14, 0);
                    spawn_flash(r, s->x, s->y, s->z, R(0.9f), false);
                }
            }
            for (int j = 0; j < MAX_SHOT && s->on; j++) {   /* bolts shoot plasma down */
                Shot *o = &r->shot[j]; if (!o->on || !o->enemy) continue;
                real dx = s->x - o->x, dy = s->y - o->y, dz = s->z - o->z;
                if (r_within3(dx, dy, dz, R(16))) { s->on = false; o->on = false; spawn_expl(r, o->x, o->y, o->z, R(0.6f)); sfx_play(13, 0); }
            }
            for (int j = 0; j < r->nprop && s->on; j++) {
                Prop *p = &r->prop[j];
                if (r_hypot(s->x - p->x, s->y - p->y) < p->r && s->z < 40 * p->scale) { s->on = false; spawn_flash(r, s->x, s->y, s->z, R(0.7f), false); }
            }
        } else {
            real dx = s->x - r->px, dy = s->y - r->py;
            if (r_within2(dx, dy, R(48))) {
                s->on = false; spawn_flash(r, s->x, s->y, s->z, R(2.5f), true);
                player_hurt(r, s->dmg, R(0.35f), "plasma");
            }
        }
    }
}

static void update_fx(Ramrod *r, real dt)
{
    for (int i = 0; i < MAX_FX; i++) {
        Fx *e = &r->fx[i]; if (!e->on) continue;
        e->t += dt;
        if (e->kind == FX_DEBRIS) {
            e->vz -= r_mul_dt(R(520), dt); e->x += r_mul_dt(e->vx, dt); e->y += r_mul_dt(e->vy, dt); e->z += r_mul_dt(e->vz, dt);
            if (e->z < 0) { e->z = 0; e->vz = r_mul(-e->vz, R(0.3f)); e->vx = r_mul(e->vx, R(0.6f)); e->vy = r_mul(e->vy, R(0.6f)); }
        } else if (e->kind == FX_SMOKE) { e->z += r_mul_dt(e->vz, dt); e->scale += r_mul(dt, R(0.8f)); }
        if (e->t >= e->dur) e->on = false;
    }
}

static int alive_mechs(const Ramrod *r) { int n = 0; for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) n++; return n; }

/* debug (SABER_R6BOT=1): a crude autopilot for balance / flow tests - faces the nearest mech, fires while locked
 * (minding the heat), punches up close, sidesteps plasma */
static void bot_input(Ramrod *r, Input *out)
{
    for (int b = 0; b < BTN_COUNT; b++) out->state[b] = 1;
    Mech *t = NULL; real td = R_MAX;
    for (int i = 0; i < MAX_MECH; i++) { Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue; real d = r_hypot(m->x - r->px, m->y - r->py); if (d < td) { td = d; t = m; } }
    static int ppress; ppress++;
    if (t) {
        real f, l; to_cam(r, t->x, t->y, &f, &l); real a = r_atan2(l, f);
        if (a > R(0.04f)) out->state[BTN_RIGHT] = 0; else if (a < R(-0.04f)) out->state[BTN_LEFT] = 0;
        if (r_abs(a) < R(0.15f) && r->heat < R(0.85f) && !r->overheated) out->state[BTN_SHOOT] = (ppress & 1) ? 0 : 0;
        if (td > R(600)) out->state[BTN_UP] = 0;
        if (td < R(280) && r_abs(a) < R(0.3f) && (ppress % 20) == 0) out->state[BTN_JUMP] = 2;
    }
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *s = &r->shot[i]; if (!s->on || !s->enemy) continue;
        real f, l; to_cam(r, s->x, s->y, &f, &l);
        if (f > 0 && f < R(400) && r_abs(l) < R(70)) { out->state[BTN_AIM] = 0; out->state[BTN_LEFT] = 1; out->state[BTN_RIGHT] = 1; out->state[l > 0 ? BTN_LEFT : BTN_RIGHT] = 0; break; }
    }
}

static void update_world(Ramrod *r, const Input *in, real dt, bool control)
{
    Input bot;
    if (control && plat_getenv("SABER_R6BOT")) { bot_input(r, &bot); in = &bot; }
    if (control) player_control(r, in, dt);
    else { r->speed = approach(r->speed, 0, r_mul_dt(R(300), dt)); r->strafe_v = approach(r->strafe_v, 0, r_mul_dt(R(400), dt)); r->turn_v = approach(r->turn_v, 0, r_mul_dt(R(7), dt)); r->bob = approach(r->bob, 0, r_mul_dt(R(20), dt)); if (r->punch_t >= 0) { r->punch_t += dt; if (r->punch_t > R(0.56f)) r->punch_t = R(-1); } }
    for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) mech_update(r, &r->mech[i], i, dt);
    update_shots(r, dt);
    update_fx(r, dt);
    update_lock(r, dt);
}

/* ---------------------------------------------------------------- flow */
void ramrod_update(Ramrod *r, const Input *in, real dt)
{
    if (!r->ok) { r->result = 1; return; }
    if (r->result) return;
    if (btn_pressed(in, BTN_PAUSE) && fighting(r)) { r->paused = !r->paused; sfx_play(10, 0); music_pause(r->paused); }
    if (r->paused) return;
    r->phase_t += dt; r->total_t += dt;
    if (r->msg_t > 0) r->msg_t -= dt;
    if (r->hurt_t > 0) r->hurt_t -= dt;
    if (r->shake > 0) r->shake -= dt;
    r->red = r_max(0, r->red - r_mul(dt, R(1.6f))); r->white = r_max(0, r->white - r_mul(dt, R(1.5f)));
    { static int last = -1; if (plat_getenv("SABER_TRACE") && r->phase != last) { fprintf(stderr, "r6 phase %d wave %d t=%s armor=%s\n", r->phase, r->wave, RS(r->total_t, 1), RS(r->armor, 0)); last = r->phase; } }
    if (r->dlg_pending) { r->dlg_pending = false; dialog_open_script(&r->dlg, r->pending_script); }

    switch (r->phase) {
    case PH_INTRO:
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { r->phase = PH_STRIDE; r->phase_t = 0; r->white = R(1.0f); sfx_play(0x13, 0); r->shake = R(0.6f); }
        update_fx(r, dt);
        break;
    case PH_STRIDE:   /* the transformation: a white flash, the cockpit shudders, POWER STRIDE */
        if (r->phase_t < R(0.8f)) r->white = r_max(r->white, R(1.0f) - r_div(r->phase_t, R(0.8f)));
        if (r->phase_t >= STRIDE_DUR) { r->phase = PH_INSTR; r->phase_t = 0; }
        break;
    case PH_INSTR: {
        bool any = false; for (int b = 0; b < BTN_COUNT; b++) if (btn_pressed(in, b)) any = true;
        if (any && r->phase_t >= INSTR_OPEN) r->phase_t = INSTR_MIN;
        if (r->phase_t >= INSTR_MIN) begin_wave(r, 0);
        if (r->phase != PH_INSTR) { r->fire_hold = r->punch_hold = true; }
        break; }
    case PH_WAVE_IN:
    case PH_FIGHT: {
        if (r->phase == PH_WAVE_IN && r->phase_t >= WAVE_IN_DUR) { r->phase = PH_FIGHT; r->phase_t = 0; }
        const Wave *w = &WAVES[r->wave];
        r->wave_t += dt;
        int active = 0; for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF && r->mech[i].st != M_DYING) active++;
        while (r->spawned < w->n && r->wave_t >= w->s[r->spawned].delay + R(1.0f) && active < w->max_active) { spawn_mech(r, &w->s[r->spawned]); r->spawned++; active++; }
        if (r->spawned < w->n && active == 0 && r->wave_t > R(1.0f)) r->wave_t = r_max(r->wave_t, w->s[r->spawned].delay + R(1.0f));   /* nobody left: bring the next one in now */
        update_world(r, in, dt, true);
        if (plat_getenv("SABER_TRACE") && r_trunc(r->total_t) != r_trunc(r->total_t - dt)) fprintf(stderr, "r6 t=%s wave=%d spawned=%d alive=%d armor=%s heat=%s pos=%s,%s h=%s kills=%d\n", RS(r->total_t, 0), r->wave, r->spawned, alive_mechs(r), RS(r->armor, 0), RS(r->heat, 2), RS(r->px, 0), RS(r->py, 0), RS(r->heading, 2), r->kills);
        if (r->phase != PH_DOWN && r->spawned >= w->n && alive_mechs(r) == 0) {
            r->phase = PH_WAVE_CLEAR; r->phase_t = 0;
            set_msg(r, r->wave == N_WAVES - 1 ? "SQUADRON DESTROYED" : "WAVE CLEARED", NULL, R(2.2f));
            for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].enemy) r->shot[i].on = false;
            if (r->wave == N_WAVES - 1) { music_play(6, false); r->music_now = 6; }
        }
        if (r->armor > 0 && r->armor < R(30)) { r->alarm_t -= dt; if (r->alarm_t <= 0) { r->alarm_t = R(1.2f); sfx_file("alarm.wav"); } }
        break; }
    case PH_WAVE_CLEAR:
        update_world(r, in, dt, false);
        if (r->phase_t >= R(2.4f)) {
            if (r->wave == N_WAVES - 1) { r->phase = PH_OUTRO; r->phase_t = 0; dialog_open_script(&r->dlg, SCRIPT_OUTRO); }
            else {
                r->phase = PH_RADIO; r->phase_t = 0; dialog_open_script(&r->dlg, *SCRIPT_BETWEEN[r->wave]);
                real heal = r->difficulty == 2 ? R(25) : R(40);   /* April patches the armour between waves; before the */
                if (r->wave == N_WAVES - 2 && r->difficulty < 2) heal = r->armor_max;   /* squadron she reroutes everything */
                r->armor = r_min(r->armor_max, r->armor + heal);
            }
        }
        break;
    case PH_RADIO:
        update_fx(r, dt);
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { begin_wave(r, r->wave + 1); r->fire_hold = r->punch_hold = true; }
        break;
    case PH_DOWN:     /* Ramrod goes down: the cockpit shakes, sparks, the screen goes dark */
        r->shake = R(0.3f); r->red = r_max(r->red, R(0.4f) + r_mul(R(0.3f), r_sin(r->phase_t * 12)));
        if (frand(r) < dt * 6) { real a = r->heading + r_mul(frand(r) - R(0.5f), R(1.0f)); spawn_expl(r, r->px + r_cos(a) * 120, r->py + r_sin(a) * 120, R(20) + frand(r) * 60, R(1.0f)); }
        update_world(r, in, dt, false);
        r->black = clampf(r_div(r->phase_t - R(1.8f), R(1.0f)), 0, R(1));
        if (r->phase_t >= R(3.2f)) {
            if (r->lives <= 0) { r->phase = PH_GAMEOVER; r->phase_t = 0; }
            else {   /* a spare: Ramrod is back on its feet, the wave starts over */
                r->lives--; r->armor = r->armor_max; r->heat = 0; r->overheated = false; r->black = 0; r->red = 0;
                begin_wave(r, r->wave);
            }
        }
        break;
    case PH_GAMEOVER:
        r->black = R(1);
        if (r->phase_t > R(1.0f)) r->result = 2;
        break;
    case PH_OUTRO:
        update_fx(r, dt);
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { r->phase = PH_CLEARED; r->phase_t = 0; }
        break;
    case PH_CLEARED:
        if (r->phase_t > R(1.2f)) r->result = 1;
        break;
    }
}

/* ---------------------------------------------------------------- drawing */
static void draw_frame(Ramrod *r, int a, int fr, real x, real y, real scale, bool flip, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    Anim *an = &r->anim[a]; if (an->n == 0) return;
    fr = fr < 0 ? 0 : fr >= an->n ? an->n - 1 : fr;
    Frame *f = &an->f[fr];
    RFRect src = { r_int(f->x), r_int(f->y), r_int(f->w), r_int(f->h) };
    real ax = r_int(flip ? f->w - 1 - f->ax : f->ax);
    RFRect dst = { r_floorr(x - r_mul(ax, scale)), r_floorr(y - f->ay * scale), r_int(r_round(f->w * scale)), r_int(r_round(f->h * scale)) };
    if (dst.w < R(1) || dst.h < R(1)) return;
    rtex_set_color_mod(r->atlas, cr, cg, cb); rtex_set_alpha_mod(r->atlas, ca);
    r_tex_rot(r->ren, r->atlas, &src, &dst, 0, NULL, flip ? R_FLIP_H : R_FLIP_NONE);
    rtex_set_color_mod(r->atlas, 255, 255, 255); rtex_set_alpha_mod(r->atlas, 255);
}

static void fill_ellipse(Ren *ren, real cx, real cy, real rx, real ry, RFColor c)
{
    enum { N = 20 }; RVertex v[N + 1]; int idx[N * 3];
    v[0].position = (RFPoint){ cx, cy }; v[0].color = c;
    for (int i = 0; i < N; i++) { real a = i * TWO_PI / N; v[i + 1].position = (RFPoint){ cx + r_mul(r_cos(a), rx), cy + r_mul(r_sin(a), ry) }; v[i + 1].color = c; }
    for (int i = 0; i < N; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = 1 + i; idx[i * 3 + 2] = 1 + (i + 1) % N; }
    r_geometry(ren, NULL, v, N + 1, idx, N * 3);
}

static void render_sky(Ramrod *r)
{
    real u = PANO_AHEAD + r_mul(r->heading - HEADING0, FOCAL) - r->sw * R(0.5f);
    u = r_fmod(u, r_int(PANO_W)); if (u < 0) u += r_int(PANO_W);
    real y0 = r->bob + (HZ - R(167.0f));
    r_set_draw_color(r->ren, 35, 131, 201, 255); RFRect top = { 0, 0, r_int(r->sw), r_max(0, y0) + R(1) }; r_fill_rect(r->ren, &top);
    for (real x = -u; x < r_int(r->sw); x += r_int(PANO_W)) { RFRect dst = { r_floorr(x), r_floorr(y0), r_int(PANO_W), r_int(PANO_H) }; r_tex(r->ren, r->sky, NULL, &dst); }
}

static void render_floor(Ramrod *r)
{
    if (!r->floor) {
        static uint8_t cell;   /* one material everywhere */
        static const uint32_t *mats[MIPS];
        for (int L = 0; L < MIPS; L++) mats[L] = r->tex[L];
        RFloorDesc d = { 1, 8, &cell, TEX, MIPS, 1, mats };
        r->floor = r_floor_create(r->ren, &d);
        if (!r->floor) return;
    }
    real hz = horizon(r);
    int y0 = r_ceil(hz + R(0.01f)), y1 = y0 + r->floor_h; if (y1 > r->sh) y1 = r->sh;
    RFloorView v = { r->px, r->py, r_cos(r->heading), r_sin(r->heading), CAM_H, FOCAL, hz, y0, y1, R(0.5f), FOG0, FOG1, 230,
                     0xFF7AAAD9u,   /* ABGR: warm dust at the horizon (d9 aa 7a) */
                     R(1.4f), r->sw };
    r_floor_draw(r->ren, r->floor, &v);
}

typedef struct { real f; int kind, i; } Item;
static int cmp_far(const void *a, const void *b) { real x = ((const Item *)a)->f, y = ((const Item *)b)->f; return x < y ? 1 : x > y ? -1 : 0; }
/* scorch marks sort behind everything (and their it->f then flattens them fully) */
#ifdef REAL_FIXED
#define BEHIND(f) (R(16384) + (f) / 4)
#else
#define BEHIND(f) ((f) + 1e5f)
#endif

static uint8_t fog_alpha(real f) { return (uint8_t)r_trunc(255 * (R(1) - clampf(r_div(f - R(2000), R(900)), 0, R(1)))); }

/* the flattening of a shadow or scorch on the sand at depth f */
static real ground_squash(real f) { return clampf(r_mul(r_div(CAM_H, f), R(1.4f)), R(0.08f), R(0.5f)); }

static void draw_mech(Ramrod *r, Mech *m, real f)
{
    real sx, sy, k; if (!project(r, m->x, m->y, 0, &sx, &sy, &k)) return;
    real sc = r_mul(r_mul(k, UNIT), m->scale);
    int fr = MF_WALK0 + (r_trunc(m->anim) & 3);
    switch (m->st) {
    case M_AIM: case M_FIRE: fr = MF_AIM; break;
    case M_WINDUP: fr = MF_WINDUP; break;
    case M_PUNCH: fr = MF_PUNCH; break;
    case M_STAGGER: fr = MF_STAGGER; break;
    default: break;
    }
    /* its shadow on the sand */
    r_set_draw_blend(r->ren, R_BLEND_BLEND);
    fill_ellipse(r->ren, sx, sy, 44 * sc, r_mul(44 * sc, ground_squash(f)), (RFColor){ R(0.2f), R(0.1f), R(0.08f), R(0.35f) });
    uint8_t cr = 255, cg = 255, cb = 255, ca = fog_alpha(f);
    if (m->flash > 0) { cr = 255; cg = 255; cb = 255; }
    if (m->st == M_DYING) {   /* burning, sinking into its own blast */
        real t = r_div(m->dying_t, R(1.5f)); uint8_t v = (uint8_t)r_trunc(R(255) - 150 * t);
        cr = v; cg = (uint8_t)r_trunc(v * R(0.7f)); cb = (uint8_t)r_trunc(v * R(0.6f));
        if (r_trunc(m->dying_t * 20) & 1) { cr = 255; cg = 200; cb = 150; }
        RRect clip = { 0, 0, r->sw, r_trunc(sy) }; r_set_clip(r->ren, &clip);
        draw_frame(r, VARIANT[m->variant].anim, MF_STAGGER, sx + r_mul(r_sin(m->dying_t * 40), R(1.5f)), sy + r_mul(r_mul(t, t) * 60, sc), sc, false, cr, cg, cb, ca);
        r_set_clip(r->ren, NULL);
        return;
    }
    draw_frame(r, VARIANT[m->variant].anim, fr, sx, sy, sc, false, cr, cg, cb, ca);
    if (m->flash > 0) {   /* hit flash: the sprite again, additive */
        rtex_set_blend(r->atlas, R_BLEND_ADD);
        draw_frame(r, VARIANT[m->variant].anim, fr, sx, sy, sc, false, 255, 255, 255, 200);
        rtex_set_blend(r->atlas, R_BLEND_BLEND);
    }
    /* the cannon charging / the fist glowing red before a punch: the telegraphs */
    if (m->st == M_AIM || m->st == M_WINDUP) {
        bool aim = m->st == M_AIM;
        real gx = sx + (aim ? 22 : 30) * sc, gy = sy - (aim ? 92 : 84) * sc;
        real p = aim ? R(1) - clampf(r_div(m->st_t, R(0.75f)), 0, R(1)) : R(1) - clampf(r_div(m->st_t, R(0.6f)), 0, R(1));
        rtex_set_blend(r->atlas, R_BLEND_ADD);
        if (aim) draw_frame(r, A_PLASMA, r_trunc(r->total_t * 16) & 3, gx, gy, r_mul(sc, R(0.4f) + r_mul(R(0.9f), p)), false, 255, 255, 255, 255);
        else draw_frame(r, A_MFLASH, r_trunc(r->total_t * 20) & 1, gx, gy, r_mul(sc, R(0.8f) + r_mul(R(1.4f), p)), false, 255, 60, 40, 255);
        rtex_set_blend(r->atlas, R_BLEND_BLEND);
    }
}

static void render_world(Ramrod *r)
{
    Item items[MAX_MECH + MAX_SHOT + MAX_FX + MAX_PROP]; int n = 0;
    for (int i = 0; i < r->nprop; i++) { real f, l; to_cam(r, r->prop[i].x, r->prop[i].y, &f, &l); if (f > R(10) && f < R(3200)) items[n++] = (Item){ f, 0, i }; }
    for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) { real f, l; to_cam(r, r->mech[i].x, r->mech[i].y, &f, &l); if (f > R(20)) items[n++] = (Item){ f, 1, i }; }
    for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].on) { real f, l; to_cam(r, r->shot[i].x, r->shot[i].y, &f, &l); if (f > R(10)) items[n++] = (Item){ f, 2, i }; }
    for (int i = 0; i < MAX_FX; i++) if (r->fx[i].on) { real f, l; to_cam(r, r->fx[i].x, r->fx[i].y, &f, &l); if (f > R(10)) items[n++] = (Item){ r->fx[i].kind == FX_SCORCH ? BEHIND(f) : f, 3, i }; }
    qsort(items, n, sizeof *items, cmp_far);
    for (int k = 0; k < n; k++) {
        Item *it = &items[k]; real sx, sy, kk;
        switch (it->kind) {
        case 0: { Prop *p = &r->prop[it->i]; if (!project(r, p->x, p->y, 0, &sx, &sy, &kk)) break;
            draw_frame(r, p->anim, 0, sx, sy, r_mul(r_mul(kk, UNIT), p->scale), p->flip, 255, 255, 255, fog_alpha(it->f)); break; }
        case 1: draw_mech(r, &r->mech[it->i], it->f); break;
        case 2: { Shot *s = &r->shot[it->i]; if (!project(r, s->x, s->y, s->z, &sx, &sy, &kk)) break;
            real qx, qy, qk;
            r_set_draw_blend(r->ren, R_BLEND_ADD);
            if (!s->enemy && project(r, s->x - r_mul(s->vx, R(0.035f)), s->y - r_mul(s->vy, R(0.035f)), s->z - r_mul(s->vz, R(0.035f)), &qx, &qy, &qk)) {   /* the bolt's streak */
                r_set_draw_color(r->ren, 255, 140, 30, 255); r_line(r->ren, qx, qy + R(1), sx, sy + R(1)); r_line(r->ren, qx + R(1), qy, sx + R(1), sy);
                r_set_draw_color(r->ren, 255, 250, 200, 255); r_line(r->ren, qx, qy, sx, sy);
            }
            rtex_set_blend(r->atlas, R_BLEND_ADD);
            if (s->enemy) draw_frame(r, A_PLASMA, r_trunc(r->total_t * 14) & 3, sx, sy, clampf(r_mul(kk, R(1.6f)), R(0.25f), R(4)), false, 255, 255, 255, 255);
            else draw_frame(r, A_BOLT, r_trunc(r->total_t * 20) & 1, sx, sy, r_max(R(0.3f), r_mul(kk, R(1.3f))), false, 255, 255, 255, 255);
            rtex_set_blend(r->atlas, R_BLEND_BLEND);
            break; }
        case 3: { Fx *e = &r->fx[it->i]; if (!project(r, e->x, e->y, e->z, &sx, &sy, &kk)) break;
            real p = r_div(e->t, e->dur), ks = r_mul(r_mul(kk, UNIT), e->scale);
            switch (e->kind) {
            case FX_EXPL: draw_frame(r, A_EXPL, r_trunc(p * 6), sx, sy, r_mul(r_mul(r_mul(kk, UNIT), R(1.4f)), e->scale), e->flip, 255, 255, 255, 255); break;
            case FX_SMOKE: draw_frame(r, A_SMOKE, r_trunc(p * 4), sx, sy, r_mul(r_mul(r_mul(kk, UNIT), R(2.0f)), e->scale), false, 255, 255, 255, (uint8_t)r_trunc(200 * (R(1) - p))); break;
            case FX_DEBRIS: draw_frame(r, A_DEBRIS, e->frame, sx, sy, ks, e->spin && (r_trunc(e->t * 8) & 1), 200, 200, 200, (uint8_t)r_trunc(255 * (R(1) - clampf(r_div(p - R(0.7f), R(0.3f)), 0, R(1))))); break;
            case FX_FLASH: rtex_set_blend(r->atlas, R_BLEND_ADD);
                /* a cockpit hit flashes right in front of the glass: capped, or the 26 px sprite grows past the screen */
                draw_frame(r, e->frame ? A_MFLASH : A_FLASH, 0, sx, sy, clampf(ks, R(0.3f), R(5)), false, 255, 255, 255, 255);
                rtex_set_blend(r->atlas, R_BLEND_BLEND); break;
            case FX_SCORCH: draw_frame(r, A_SCORCH, 0, sx, sy + r_mul(r_mul(r_mul(12 * kk, UNIT), e->scale), ground_squash(it->f)), ks, false, 255, 255, 255, 100); break;
            }
            break; }
        }
    }
}

/* the reticle, lock brackets and the edge-of-screen threat markers */
static void render_aim(Ramrod *r)
{
    Ren *ren = r->ren; real cx = r->sw * R(0.5f), cy = AIM_Y + r->bob / 2;
    bool locked = r->lock >= 0;
    r_set_draw_blend(ren, R_BLEND_BLEND);
    if (locked) {
        Mech *m = &r->mech[r->lock]; real sx, sy, k;
        if (project(r, m->x, m->y, 0, &sx, &sy, &k)) {
            real sc = r_mul(r_mul(k, UNIT), m->scale), hw = 40 * sc + R(3), top = sy - 140 * sc, bot = sy - 6 * sc;
            real L = clampf(hw / 2, R(3), R(10)), shrink = r_max(0, R(1) - r->lock_t * 5) * 12;
            real x0 = r_floorr(sx - hw - shrink), x1 = r_floorr(sx + hw + shrink), y0 = r_floorr(top - shrink), y1 = r_floorr(bot + shrink);
            r_set_draw_color(ren, 255, 60, 60, 255);
            RFRect q[8] = { { x0, y0, L, R(1) }, { x0, y0, R(1), L }, { x1 - L, y0, L, R(1) }, { x1, y0, R(1), L }, { x0, y1, L, R(1) }, { x0, y1 - L, R(1), L }, { x1 - L, y1, L, R(1) }, { x1, y1 - L, R(1), L } };
            r_fill_rects(ren, q, 8);
            /* its armour, over the brackets */
            real f = clampf(r_div(m->hp, m->hp_max), 0, R(1)), w = r_max(R(16), x1 - x0);
            r_set_draw_color(ren, 0, 0, 0, 160); RFRect bg = { x0, y0 - R(5), w, R(3) }; r_fill_rect(ren, &bg);
            r_set_draw_color(ren, 255, 90, 60, 255); RFRect fg = { x0, y0 - R(5), r_mul(w, f), R(3) }; r_fill_rect(ren, &fg);
        }
    }
    uint8_t cr = locked ? 255 : 120, cg = locked ? 70 : 255, cb = locked ? 60 : 140;
    r_set_draw_color(ren, cr, cg, cb, 230);
    real g = locked ? R(3) : R(5);
    RFRect q[4] = { { cx - g - R(6), cy, R(6), R(1) }, { cx + g + R(1), cy, R(6), R(1) }, { cx, cy - g - R(6), R(1), R(6) }, { cx, cy + g + R(1), R(1), R(6) } };
    r_fill_rects(ren, q, 4);
    RFRect dot = { cx, cy, R(1), R(1) }; r_fill_rect(ren, &dot);
    /* threats outside the view: a chevron on that side, blinking when it is about to fire or swing */
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
        real f, l; to_cam(r, m->x, m->y, &f, &l);
        real a = r_atan2(l, f);
        if (r_abs(a) < R(0.82f)) continue;
        bool danger = m->st == M_AIM || m->st == M_WINDUP || m->st == M_CHARGE;
        if (danger && (r_trunc(r->total_t * 8) & 1)) continue;
        real x = a > 0 ? r_int(r->sw) - R(12.0f) : R(6.0f), y = R(120) + clampf(r_abs(a) - R(0.8f), 0, R(2.4f)) * 10;
        r_set_draw_color(ren, 255, danger ? 60 : 200, 60, 255);
        for (int k = 0; k < 5; k++) {   /* a solid arrowhead pointing out of the screen */
            real w = r_int(5 - k); RFRect c = { a < 0 ? x + r_int(k) : x + R(5) - r_int(k), y - w, R(1), w * 2 }; r_fill_rect(ren, &c);
        }
    }
}

/* Ramrod's arm reaching out past the windshield: the clip's frames played out, held, and pulled back; the right
 * fist is the left one mirrored */
static void render_arm(Ramrod *r)
{
    if (r->punch_t < 0) return;
    real t = r->punch_t; int fr;
    if (t < R(0.28f)) fr = r_trunc(r_div(t, R(0.28f)) * 10); else if (t < R(0.36f)) fr = 9; else fr = 9 - r_trunc(r_div(t - R(0.36f), R(0.2f)) * 10);
    if (fr < 0) return;
    if (fr > 9) fr = 9;
    Frame *f = &r->anim[A_ARM].f[fr];
    real x = xoff(r) - r_int(f->ax), y = r_int(-f->ay) + r_mul(r->bob, R(0.3f));
    bool right = r->punch_side == 1;
    if (right) x = r_int(r->sw) - (x + r_int(f->w));
    RFRect src = { r_int(f->x), r_int(f->y), r_int(f->w), r_int(f->h) }, dst = { r_floorr(x), r_floorr(y), r_int(f->w), r_int(f->h) };
    r_tex_rot(r->ren, r->atlas, &src, &dst, 0, NULL, right ? R_FLIP_H : R_FLIP_NONE);
}

static void bar(Ren *ren, real x, real y, real w, real h, real f, uint8_t cr, uint8_t cg, uint8_t cb)
{
    r_set_draw_blend(ren, R_BLEND_BLEND);
    r_set_draw_color(ren, 0, 0, 0, 170); RFRect bg = { x, y, w, h }; r_fill_rect(ren, &bg);
    r_set_draw_color(ren, cr, cg, cb, 255); RFRect fg = { x, y, r_floorr(r_mul(w, clampf(f, 0, R(1)))), h }; r_fill_rect(ren, &fg);
}

/* the two monitors hanging from the canopy: radar left, Ramrod's status right */
static void render_monitors(Ramrod *r)
{
    Ren *ren = r->ren; real ox = xoff(r);
    Font *small = font_get(0x12072E60);
    /* radar: forward is up, 1 px = 60 units */
    RFRect scr = { ox + R(118), R(23), R(52), R(33) };
    r_set_draw_blend(ren, R_BLEND_NONE);
    r_set_draw_color(ren, 6, 34, 20, 255); r_fill_rect(ren, &scr);
    real cx = scr.x + scr.w / 2, cy = scr.y + scr.h / 2 + R(2);
    r_set_draw_color(ren, 20, 90, 50, 255);
    for (int k = 1; k <= 2; k++) for (int i = 0; i < 40; i++) { real a = i * TWO_PI / 40; r_point(ren, cx + r_cos(a) * 8 * k, cy + r_sin(a) * 8 * k); }
    r_line(ren, cx, cy, cx - R(12), cy - R(14)); r_line(ren, cx, cy, cx + R(12), cy - R(14));   /* the view cone */
    real sweep = r_mul(r->total_t, R(3.0f));
    r_set_draw_color(ren, 60, 200, 110, 255); r_line(ren, cx, cy, cx + r_sin(sweep) * 16, cy - r_cos(sweep) * 16);
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF) continue;
        real f, l; to_cam(r, m->x, m->y, &f, &l);
        real bx = clampf(cx + l / 60, scr.x + R(1), scr.x + scr.w - R(3)), by = clampf(cy - f / 60, scr.y + R(1), scr.y + scr.h - R(3));
        bool blink = (m->st == M_AIM || m->st == M_WINDUP || m->st == M_CHARGE) && (r_trunc(r->total_t * 8) & 1);
        if (m->st == M_DYING) r_set_draw_color(ren, 90, 90, 90, 255);
        else if (m->variant == V_COMMANDER) r_set_draw_color(ren, 255, 210, 40, 255);
        else if (m->variant == V_HEAVY) r_set_draw_color(ren, 255, 70, 60, 255);
        else r_set_draw_color(ren, 120, 255, 200, 255);
        if (blink) r_set_draw_color(ren, 255, 255, 255, 255);
        RFRect b = { r_floorr(bx), r_floorr(by), m->variant == V_COMMANDER ? R(3.0f) : R(2.0f), m->variant == V_COMMANDER ? R(3.0f) : R(2.0f) }; r_fill_rect(ren, &b);
    }
    r_set_draw_color(ren, 255, 110, 230, 255);
    for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].on && r->shot[i].enemy) { real f, l; to_cam(r, r->shot[i].x, r->shot[i].y, &f, &l); real bx = cx + l / 60, by = cy - f / 60; if (bx > scr.x && bx < scr.x + scr.w && by > scr.y && by < scr.y + scr.h) r_point(ren, bx, by); }
    r_set_draw_color(ren, 255, 255, 255, 255); RFRect me = { cx - R(1), cy - R(1), R(2), R(2) }; r_fill_rect(ren, &me);
    /* status */
    RFRect st = { ox + R(259), R(23), R(52), R(33) };
    r_set_draw_color(ren, 18, 22, 60, 255); r_fill_rect(ren, &st);
    real ar = r_div(r->armor, r->armor_max);
    bool blink = ar < R(0.3f) && (r_trunc(r->total_t * 4) & 1);
    if (small) {
        font_draw(small, "ARM", st.x + R(2), st.y + R(1), 255, blink ? 80 : 210, blink ? 80 : 120);
        font_draw(small, r->overheated ? "HOT" : "GUN", st.x + R(2), st.y + R(11), 255, r->overheated ? 80 : 210, r->overheated ? 60 : 120);
    }
    /* the bars start past the widest label so they never run into the text */
    real lw = R(20);
    if (small) { const char *L[3] = { "ARM", "GUN", "HOT" }; for (int i = 0; i < 3; i++) { real w = r_int(font_text_width(small, L[i])); if (w > lw) lw = w; } }
    real bx = st.x + R(2) + lw + R(3), bw = st.x + st.w - R(2) - bx;
    bar(ren, bx, st.y + R(3), bw, R(5), ar, ar > R(0.5f) ? 90 : ar > R(0.25f) ? 240 : 250, ar > R(0.5f) ? 220 : ar > R(0.25f) ? 190 : 60, 60);
    if (r->hurt_t > 0 && (r_trunc(r->hurt_t * 20) & 1)) bar(ren, bx, st.y + R(3), bw, R(5), R(1), 255, 255, 255);
    bar(ren, bx, st.y + R(13), bw, R(5), r->heat, r->overheated ? 255 : 255, r->overheated ? 60 : 160 - (uint8_t)r_trunc(100 * r->heat), 40);
    if (small) {
        char buf[32]; int left = WAVES[r->wave].n - r->spawned + alive_mechs(r);
        for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st == M_DYING) left--;
        snprintf(buf, sizeof buf, "W%d", r->wave + 1); font_draw(small, buf, st.x + R(2), st.y + R(22), 150, 200, 255);
        snprintf(buf, sizeof buf, "%d", left < 0 ? 0 : left); font_draw(small, buf, st.x + R(20), st.y + R(22), 255, 120, 120);
        snprintf(buf, sizeof buf, "x%d", r->lives); font_draw(small, buf, st.x + R(50) - r_int(font_text_width(small, buf)), st.y + R(22), 255, 255, 255);
    }
    /* static on the screens when hit */
    if (r->hurt_t > R(0.25f)) {
        for (int i = 0; i < 90; i++) {
            RFRect *q = i & 1 ? &scr : &st; uint8_t v = (uint8_t)r_trunc(frand(r) * 255);
            r_set_draw_color(ren, v, v, v, 255); r_point(ren, q->x + r_mul(frand(r), q->w), q->y + r_mul(frand(r), q->h));
        }
    }
}

static void render_instructions(Ramrod *r, Font *f, Font *small)
{
    real t = r->phase_t; int sw = r->sw, sh = r->sh;
    real open = clampf(r_div(t, INSTR_OPEN), 0, R(1)); open = R(1) - r_mul(R(1) - open, R(1) - open);
    r_set_draw_blend(r->ren, R_BLEND_BLEND);
    r_set_draw_color(r->ren, 0, 0, 0, (uint8_t)r_trunc(140 * open)); RFRect scrim = { 0, 0, r_int(sw), r_int(sh) }; r_fill_rect(r->ren, &scrim);
    static const struct { const char *label, *desc; } LINES[] = {
        { "TURN", "Left / Right" }, { "WALK", "Up / Down" }, { "SIDESTEP", "Aim button + Left / Right" },
        { "GUNS", "Shoot button (they overheat)" }, { "PUNCH", "Jump button - close range" },
    };
    int nl = (int)(sizeof LINES / sizeof *LINES);
    /* 4:3 has no room for the wide layout: the panel widens to the edges and descriptions / the tip wrap */
    bool narrow = sw < 400;
    real x0 = narrow ? R(8) : R(20), pw = r_int(sw) - 2 * x0, lx = narrow ? R(8) : R(14), dx = R(104);
    const char *sub = "PUNCH A MECH AS IT WINDS UP TO COUNTER";
    char desc[8][3][96], subl[3][96]; int dn[8], sn = 1;
    if (small) {
        if (narrow) { dx = 0; for (int i = 0; i < nl; i++) dx = r_max(dx, r_int(font_text_width(small, LINES[i].label))); dx += lx + R(10); }
        for (int i = 0; i < nl; i++) dn[i] = font_wrap(small, LINES[i].desc, pw - dx - R(6), desc[i], 3);
        sn = font_wrap(small, sub, pw - R(12), subl, 3);
    } else for (int i = 0; i < nl; i++) dn[i] = 1;
    real rows_h = 0; for (int i = 0; i < nl; i++) rows_h += r_int(14 + (dn[i] - 1) * 9);
    real full_h = R(30) + rows_h + R(4) + r_int(sn * 10) + R(12), ph = r_mul(full_h, open), y0 = (r_int(sh) - full_h) / 2 + (full_h - ph) / 2;
    r_set_draw_color(r->ren, 10, 18, 44, (uint8_t)r_trunc(255 * open)); RFRect panel = { x0, y0, pw, ph }; r_fill_rect(r->ren, &panel);
    r_set_draw_color(r->ren, 60, 120, 220, (uint8_t)r_trunc(255 * open));
    RFRect top = { x0, y0 - R(2), pw, R(2) }, bot = { x0, y0 + ph, pw, R(2) }; r_fill_rect(r->ren, &top); r_fill_rect(r->ren, &bot);
    if (open < R(1) || !f || !small) return;
    const char *title = "RAMROD - ROBOT MODE";
    font_draw(f, title, x0 + (pw - r_int(font_text_width(f, title))) / 2, y0 + R(8), 255, 182, 0);
    real y = y0 + R(30);
    for (int i = 0; i < nl; i++) {
        font_draw(small, LINES[i].label, x0 + lx, y, 255, 224, 192);
        for (int k = 0; k < dn[i]; k++) font_draw(small, desc[i][k], x0 + dx, y + r_int(k * 9), 220, 230, 255);
        y += r_int(14 + (dn[i] - 1) * 9);
    }
    y += R(4);
    for (int k = 0; k < sn; k++, y += R(10)) font_draw(small, subl[k], x0 + (pw - r_int(font_text_width(small, subl[k]))) / 2, y, 255, 255, 255);
    if (t >= INSTR_OPEN && (r_trunc(t * 4) & 1)) { const char *s = "PRESS A BUTTON"; font_draw(small, s, x0 + (pw - r_int(font_text_width(small, s))) / 2, y, 200, 200, 200); }
}

static void center_text(Ramrod *r, Font *f, const char *s, real y, uint8_t cr, uint8_t cg, uint8_t cb)
{
    real x = r->sw * R(0.5f) - r_int(font_text_width(f, s)) / 2;
    font_draw(f, s, x + R(1), y + R(1), 0, 0, 0); font_draw(f, s, x, y, cr, cg, cb);
}

void ramrod_draw(Ramrod *r, bool scanlines)
{
    if (!r->ok) return;
    Ren *ren = r->ren;
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    real shx = 0, shy = 0;
    if (r->shake > 0) { shx = r_mul((frand(r) - R(0.5f)) * 6, r_min(R(1), r->shake * 3)); shy = r_mul((frand(r) - R(0.5f)) * 5, r_min(R(1), r->shake * 3)); }
    real bob = r->bob; r->bob += shy;   /* the shake moves the world; the cockpit only jolts a little */
    RRect vp = { r_trunc(shx), 0, r->sw, r->sh }; r_set_viewport(ren, &vp);
    render_sky(r);
    render_floor(r);
    render_world(r);
    r->bob = bob;
    r_set_viewport(ren, NULL);
    if (fighting(r) || r->phase == PH_WAVE_CLEAR) render_aim(r);
    render_arm(r);
    RFRect cp = { r_floorr(xoff(r) + r_mul(shx, R(0.3f))), r_floorr(r_mul(shy, R(0.3f))), r_int(ART_W), R(240) };
    r_tex(ren, r->cockpit, NULL, &cp);
    render_monitors(r);
    r_set_draw_blend(ren, R_BLEND_BLEND);
    if (r->red > 0) { r_set_draw_color(ren, 200, 20, 10, (uint8_t)r_trunc(110 * clampf(r->red, 0, R(1)))); RFRect q = { 0, 0, r_int(r->sw), r_int(r->sh) }; r_fill_rect(ren, &q); }
    if (f && small) {
        if (r->phase == PH_STRIDE) {
            real t = r->phase_t;
            if (t > R(0.5f)) center_text(r, f, "POWER STRIDE!", R(70), 255, 210, 60);
            if (t > R(1.1f)) center_text(r, small, "RAMROD - ROBOT MODE", R(90), 255, 255, 255);
        }
        if (r->phase == PH_WAVE_IN && r->phase_t < WAVE_IN_DUR) {
            const Wave *w = &WAVES[r->wave];
            if ((r_trunc(r->phase_t * 4) & 1) || r->phase_t > R(1.2f)) center_text(r, f, w->title, R(64), 255, 182, 0);
            center_text(r, small, w->sub, R(84), 255, 255, 255);
            if (r->wave == N_WAVES - 1 && r->phase_t > R(1.0f)) center_text(r, small, "WARNING: COMMAND MECH", R(96), 255, 80, 80);
        }
        if (r->msg_t > 0 && r->phase != PH_WAVE_IN) { center_text(r, f, r->msg, R(70), 255, 255, 255); if (r->msg2[0]) center_text(r, small, r->msg2, R(88), 255, 200, 160); }
        if (r->phase == PH_DOWN && r->phase_t > R(0.6f)) center_text(r, f, "RAMROD IS DOWN!", R(70), 255, 60, 60);
        if (r->paused) center_text(r, f, "PAUSE", R(100), 255, 255, 255);
    }
    if (r->dlg.active) dialog_draw(&r->dlg, ren, r->sw, r->sh);
    if (r->phase == PH_INSTR) render_instructions(r, f, small);
    if (r->white > 0) { r_set_draw_color(ren, 255, 255, 255, (uint8_t)r_trunc(255 * clampf(r->white, 0, R(1)))); RFRect q = { 0, 0, r_int(r->sw), r_int(r->sh) }; r_fill_rect(ren, &q); }
    if (scanlines) gfx_scanlines(r->sw, r->sh);
    real fade = r->phase == PH_CLEARED ? clampf(r_div(r->phase_t, R(1.2f)), 0, R(1)) : r->black;
    if (fade > 0) {
        uint8_t v = r->phase == PH_CLEARED ? 255 : 0;
        r_set_draw_color(ren, v, v, v, (uint8_t)r_trunc(255 * fade)); RFRect q = { 0, 0, r_int(r->sw), r_int(r->sh) }; r_fill_rect(ren, &q);
    }
}
