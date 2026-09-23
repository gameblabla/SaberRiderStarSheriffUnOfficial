#include "ramrod.h"
#include "assets.h"
#include "font.h"
#include "audio.h"
#include "dialog.h"
#include "heroes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PI 3.1415927f
#define TWO_PI 6.2831853f

/* ---- view: the cockpit art is a 426x240 screen; row 167 is the first row of sand in the clip ---- */
#define ART_W 426
#define HZ 167.0f            /* screen row of the horizon */
#define FOCAL 213.9f         /* px; the panorama is 1344 px = 360 degrees at this focal length */
#define PANO_W 1344
#define PANO_H 170
#define PANO_AHEAD 513.0f    /* panorama x straight ahead at the start heading (the planet) */
#define HEADING0 (-PI / 2)   /* north: towards the ridge the Renegades come over */
#define CAM_H 45.0f          /* Ramrod's eye height, world units: low enough that a mech's chest stands above the seats */
#define UNIT 0.85f           /* world units per sprite pixel at scale 1 (the mech canvas: ~110 units tall) */
#define AIM_Y 160.0f         /* the reticle's row */
#define FOG0 700.0f
#define FOG1 2600.0f
#define TEX 256              /* floor texture, 1 texel = 1 unit */
#define MIPS 4

/* ---- world ---- */
#define ARENA_R 1900.0f      /* the battle zone around the outpost */
#define PLAYER_R 60.0f
#define MECH_R 34.0f
#define MECH_H 115.0f
#define PUNCH_RANGE 250.0f   /* centre to centre */
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
static const struct { int anim; float hp, speed, scale; int volley; float shot_dmg, punch_dmg, rest, aim; const char *name; } VARIANT[3] = {
    { A_MECH,      16, 76, 1.00f, 1, 8, 14, 1.0f, 0.70f, "RENEGADE" },
    { A_MECH_RED,  34, 84, 1.06f, 3, 6, 16, 1.1f, 0.85f, "RENEGADE HEAVY" },
    { A_MECH_GOLD, 90, 78, 1.30f, 4, 8, 22, 0.5f, 0.70f, "RENEGADE COMMANDER" },
};
enum { M_OFF, M_ENTER, M_APPROACH, M_CIRCLE, M_AIM, M_FIRE, M_CHARGE, M_WINDUP, M_PUNCH, M_STAGGER, M_DYING };
typedef struct {
    int st, variant; float x, y, hp, hp_max, st_t, anim, strafe, pref, kvx, kvy, flash, scale, fire_t, dying_t, expl_t;
    int volley_left; bool step_sfx;
    float punch_dir;   /* where the swing goes: locked when it winds up, so a sidestep makes it miss */
} Mech;

typedef struct { bool on, enemy; float x, y, z, vx, vy, vz, life, px, py, pz, dmg; int side; } Shot;
enum { FX_EXPL, FX_SMOKE, FX_DEBRIS, FX_FLASH, FX_SCORCH };
typedef struct { bool on; int kind, frame; float x, y, z, vx, vy, vz, t, dur, scale, spin; bool flip; } Fx;
typedef struct { int anim; float x, y, scale, r; bool flip; } Prop;

/* ---- waves ---- */
typedef struct { int variant; float ang, delay; } Spawn;
typedef struct { const char *title, *sub; int n, max_active; Spawn s[9]; } Wave;
static const Wave WAVES[3] = {
    { "WAVE 1", "OUTRIDER VANGUARD", 4, 2, { { V_GRUNT, 0.0f, 0.0f }, { V_GRUNT, 0.45f, 2.0f }, { V_GRUNT, -0.5f, 6.0f }, { V_GRUNT, 0.2f, 10.0f } } },
    { "WAVE 2", "THE PINCER", 6, 3, { { V_GRUNT, -1.3f, 0.0f }, { V_GRUNT, 1.3f, 0.5f }, { V_HEAVY, 0.1f, 4.0f },
                                     { V_GRUNT, 3.0f, 7.0f }, { V_HEAVY, -2.2f, 10.0f }, { V_GRUNT, -0.4f, 12.0f } } },
    { "WAVE 3", "RENEGADE SQUADRON", 8, 3, { { V_COMMANDER, 0.0f, 0.0f }, { V_GRUNT, -0.6f, 0.3f }, { V_GRUNT, 0.6f, 0.6f },
                                     { V_HEAVY, 2.4f, 6.0f }, { V_GRUNT, -2.4f, 8.0f }, { V_HEAVY, 3.1f, 13.0f }, { V_GRUNT, 1.6f, 15.0f },
                                     { V_HEAVY, -1.2f, 18.0f } } },
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
static const char *const SCRIPT_OUTRO =
    "<|PURPLE|>\n</dialog_avatar_outrider/>\nEnjoy it, Star Sheriffs... we were only the vanguard. The ground will shake when the Renegade comes for your outpost.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_saber2/>\nThe town is safe - for now. Ramrod, hold the line. Whatever comes over that ridge, we'll be waiting.\n<<>>\n"
    "<|GREEN|>\n</dialog_avatar_fireball1/>\nYou heard the man. Nobody rides into Yuma past us.\n<<>>\n";
static const char *const *const SCRIPT_BETWEEN[N_WAVES] = { &SCRIPT_AFTER1, &SCRIPT_AFTER2, NULL };

enum { PH_INTRO, PH_STRIDE, PH_INSTR, PH_WAVE_IN, PH_FIGHT, PH_WAVE_CLEAR, PH_RADIO, PH_DOWN, PH_GAMEOVER, PH_OUTRO, PH_CLEARED };
#define STRIDE_DUR 2.2f
#define INSTR_OPEN 0.5f
#define INSTR_MIN 2.4f
#define WAVE_IN_DUR 2.6f

struct Ramrod {
    SDL_Renderer *ren; int sw, sh; bool ok;
    SDL_Texture *atlas, *sky, *cockpit, *floor_tex; Anim anim[A_COUNT];
    uint32_t tex[MIPS][TEX * TEX]; uint32_t *floor_px; int floor_h;
    int difficulty, lives, result;
    /* Ramrod */
    float px, py, heading, turn_v, speed, strafe_v, armor, armor_max, heat, fire_cd, gun_idle; bool overheated; int gun_side;
    float step_phase, bob, shake, hurt_t, alarm_t; bool fire_hold, punch_hold;
    float punch_t; int punch_side; bool punch_hit_done; float punch_cd;
    int lock; float lock_t;
    /* world */
    Mech mech[MAX_MECH]; Shot shot[MAX_SHOT]; Fx fx[MAX_FX]; Prop prop[MAX_PROP]; int nprop;
    /* flow */
    int phase, wave, spawned, kills; float phase_t, wave_t, total_t;
    Dialog dlg; bool paused, dlg_pending; const char *pending_script;
    char msg[48], msg2[48]; float msg_t;
    float white, red, black;
    unsigned rng; int music_now;
    bool god;
};

/* ---------------------------------------------------------------- helpers */
static float frand(Ramrod *r) { r->rng = r->rng * 1664525u + 1013904223u; return (r->rng >> 8) / 16777216.0f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float approach(float v, float t, float d) { return v < t ? fminf(v + d, t) : fmaxf(v - d, t); }
static float xoff(const Ramrod *r) { return (r->sw - ART_W) * 0.5f; }
static void set_msg(Ramrod *r, const char *a, const char *b, float t) { snprintf(r->msg, sizeof r->msg, "%s", a); snprintf(r->msg2, sizeof r->msg2, "%s", b ? b : ""); r->msg_t = t; }
static void play_music(Ramrod *r, int track) { if (r->music_now != track) { music_play(track, true); r->music_now = track; music_set_volume(1.0f); } }
static void sfx_file(const char *name) { char buf[64]; snprintf(buf, sizeof buf, "ramrod/%s", name); sfx_play_file(asset_path(buf)); }
static float dmg_mul(const Ramrod *r) { return r->difficulty == 0 ? 0.6f : r->difficulty == 1 ? 1.0f : 1.35f; }

/* camera space: f = distance ahead, l = to the right */
static void to_cam(const Ramrod *r, float x, float y, float *f, float *l)
{
    float dx = x - r->px, dy = y - r->py, c = cosf(r->heading), s = sinf(r->heading);
    *f = dx * c + dy * s; *l = -dx * s + dy * c;
}
static float horizon(const Ramrod *r) { return HZ + r->bob; }
static bool project(const Ramrod *r, float x, float y, float z, float *sx, float *sy, float *k)
{
    float f, l; to_cam(r, x, y, &f, &l);
    if (f < 8) return false;
    *sx = r->sw * 0.5f + l * FOCAL / f; *sy = horizon(r) + (CAM_H - z) * FOCAL / f; if (k) *k = FOCAL / f;
    return true;
}

static Fx *fx_new(Ramrod *r) { for (int i = 0; i < MAX_FX; i++) if (!r->fx[i].on) { Fx *e = &r->fx[i]; memset(e, 0, sizeof *e); e->on = true; e->scale = 1; return e; } return NULL; }
static void spawn_expl(Ramrod *r, float x, float y, float z, float scale)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_EXPL; e->x = x; e->y = y; e->z = z; e->scale = scale; e->dur = 0.5f; e->flip = frand(r) < 0.5f;
}
static void spawn_flash(Ramrod *r, float x, float y, float z, float scale, bool enemy)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_FLASH; e->x = x; e->y = y; e->z = z; e->scale = scale; e->dur = 0.09f; e->frame = enemy ? 1 : 0;
}
static void spawn_smoke(Ramrod *r, float x, float y, float z, float scale)
{
    Fx *e = fx_new(r); if (!e) return;
    e->kind = FX_SMOKE; e->x = x; e->y = y; e->z = z; e->vz = 30 + frand(r) * 20; e->scale = scale; e->dur = 1.1f;
}

/* ---------------------------------------------------------------- loading */
static SDL_Texture *load_tex(Ramrod *r, const char *name, int *w, int *h, uint32_t **keep)
{
    char buf[64]; snprintf(buf, sizeof buf, "ramrod/%s", name);
    const char *p = asset_path(buf);
    if (!p) { fprintf(stderr, "assets/%s missing (run ../ramrod/build.py)\n", buf); return NULL; }
    int ww, hh; uint32_t *px = png_load_rgba(p, &ww, &hh);
    if (!px) return NULL;
    SDL_Texture *t = SDL_CreateTexture(r->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, ww, hh);
    SDL_UpdateTexture(t, NULL, px, ww * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    if (w) *w = ww;
    if (h) *h = hh;
    if (keep) *keep = px; else free(px);
    return t;
}

static bool load_assets(Ramrod *r)
{
    int w, h;
    r->atlas = load_tex(r, "atlas.png", NULL, NULL, NULL);
    r->sky = load_tex(r, "sky.png", &w, &h, NULL);
    r->cockpit = load_tex(r, "cockpit.png", NULL, NULL, NULL);
    uint32_t *fl = NULL; SDL_Texture *ft = load_tex(r, "floor.png", &w, &h, &fl);
    if (ft) SDL_DestroyTexture(ft);
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
    FILE *f = p ? fopen(p, "r") : NULL;
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
        float a = frand(r) * TWO_PI, d = 450 + frand(r) * (ARENA_R + 600 - 450);
        Prop *p = &r->prop[r->nprop++];
        float u = frand(r);
        p->anim = u < 0.35f ? A_ROCK_BIG : u < 0.65f ? A_ROCK_SMALL : u < 0.8f ? A_CACTUS : A_MESA;
        p->scale = p->anim == A_MESA ? 2.2f + frand(r) : p->anim == A_ROCK_BIG ? 1.1f + frand(r) * 0.6f : p->anim == A_CACTUS ? 0.7f : 1.0f + frand(r) * 0.5f;
        if (p->anim == A_MESA) d = ARENA_R + 300 + frand(r) * 600;   /* the buttes stand at the zone's edge */
        p->x = cosf(a) * d; p->y = sinf(a) * d; p->flip = frand(r) < 0.5f;
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

Ramrod *ramrod_create(SDL_Renderer *ren, int sw, int sh, int difficulty, int lives)
{
    Ramrod *r = calloc(1, sizeof *r);
    r->ren = ren; r->sw = sw; r->sh = sh; r->rng = 0x5AB3E7u; r->music_now = -1;
    r->floor_h = sh - (int)HZ + 8;
    r->floor_px = calloc((size_t)sw * r->floor_h, 4);
    r->floor_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, sw, r->floor_h);
    SDL_SetTextureScaleMode(r->floor_tex, SDL_SCALEMODE_NEAREST);
    r->ok = load_assets(r);
    r->difficulty = difficulty; r->lives = lives;
    r->armor = r->armor_max = 100; r->heading = HEADING0; r->lock = -1; r->punch_t = -1;
    r->god = SDL_getenv("SABER_R6GOD") != NULL;
    dialog_set_hero(HERO_FIREBALL);   /* Ramrod is everyone's: the scenes are written for the whole crew */
    place_props(r);
    r->phase = PH_INTRO; r->dlg_pending = true; r->pending_script = SCRIPT_INTRO;
    play_music(r, 16);
    if (SDL_getenv("SABER_R6WAVE")) {   /* debug: straight into wave n (1..3), no story */
        int w = atoi(SDL_getenv("SABER_R6WAVE")) - 1; r->dlg_pending = false;
        begin_wave(r, w < 0 ? 0 : w >= N_WAVES ? N_WAVES - 1 : w);
    }
    return r;
}

void ramrod_destroy(Ramrod *r)
{
    if (!r) return;
    if (r->atlas) SDL_DestroyTexture(r->atlas);
    if (r->sky) SDL_DestroyTexture(r->sky);
    if (r->cockpit) SDL_DestroyTexture(r->cockpit);
    if (r->floor_tex) SDL_DestroyTexture(r->floor_tex);
    free(r->floor_px); free(r);
}
int ramrod_result(const Ramrod *r) { return r->result; }
int ramrod_lives(const Ramrod *r) { return r->lives; }

/* ---------------------------------------------------------------- combat */
static bool fighting(const Ramrod *r) { return r->phase == PH_FIGHT || r->phase == PH_WAVE_IN; }

static void player_hurt(Ramrod *r, float dmg, float shake, const char *what)
{
    if (!fighting(r) || r->god) return;
    if (SDL_getenv("SABER_TRACE")) fprintf(stderr, "r6 hurt %s %.0f (armor %.0f)\n", what, dmg * dmg_mul(r), r->armor);
    r->armor -= dmg * dmg_mul(r); r->hurt_t = 0.5f; r->shake = fmaxf(r->shake, shake); r->red = fmaxf(r->red, 0.55f);
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

static void mech_damage(Ramrod *r, Mech *m, float dmg, bool punch)
{
    if (m->st == M_DYING || m->st == M_OFF) return;
    m->hp -= dmg; m->flash = 0.12f;
    if (m->hp <= 0) { mech_kill(r, m); return; }
    if (punch) {   /* a fist rocks it back; one caught winding up loses its punch */
        float c = cosf(r->heading), s = sinf(r->heading);
        bool was_windup = m->st == M_WINDUP;
        m->kvx = c * 300; m->kvy = s * 300; m->st = M_STAGGER; m->st_t = was_windup ? 1.0f : 0.75f;
    }
}

static void fire_bolt(Ramrod *r)
{
    Shot *s = NULL; for (int i = 0; i < MAX_SHOT; i++) if (!r->shot[i].on) { s = &r->shot[i]; break; }
    if (!s) return;
    memset(s, 0, sizeof *s);
    float c = cosf(r->heading), sn = sinf(r->heading), side = r->gun_side ? 1.0f : -1.0f;
    r->gun_side ^= 1;
    /* Ramrod's shoulder guns: below and to either side of the view, converging on the reticle */
    s->x = r->px + c * 50 - sn * side * 44; s->y = r->py + sn * 50 + c * side * 44; s->z = CAM_H - 30;
    float tx, ty, tz;
    if (r->lock >= 0) { Mech *m = &r->mech[r->lock]; tx = m->x; ty = m->y; tz = 70 * m->scale; }
    else { tx = r->px + c * 900; ty = r->py + sn * 900; tz = CAM_H - (HZ - AIM_Y) * 900 / FOCAL; }
    float dx = tx - s->x, dy = ty - s->y, dz = tz - s->z, d = sqrtf(dx * dx + dy * dy + dz * dz);
    const float V = 1500;
    s->vx = dx / d * V; s->vy = dy / d * V; s->vz = dz / d * V; s->life = 1.2f; s->on = true; s->side = side > 0;
    s->px = s->x; s->py = s->y; s->pz = s->z;
    sfx_file("laser.wav");
}

static void fire_plasma(Ramrod *r, Mech *m)
{
    Shot *s = NULL; for (int i = 0; i < MAX_SHOT; i++) if (!r->shot[i].on) { s = &r->shot[i]; break; }
    if (!s) return;
    memset(s, 0, sizeof *s);
    /* from the cannon fist: right of the mech as the player sees it, at chest height */
    float dx = r->px - m->x, dy = r->py - m->y, d = hypotf(dx, dy); if (d < 1) d = 1;
    float rx = -dy / d, ry = dx / d;   /* the viewer's right, seen from the mech: its left... */
    s->x = m->x - rx * 22 * m->scale; s->y = m->y - ry * 22 * m->scale; s->z = 78 * m->scale;
    if (m->variant != V_GRUNT) {   /* the veterans lead a moving target */
        float c = cosf(r->heading), sn = sinf(r->heading), t = d / 410.0f;
        dx += (c * r->speed - sn * r->strafe_v) * t * 0.8f; dy += (sn * r->speed + c * r->strafe_v) * t * 0.8f;
    }
    float spread = m->volley_left > 0 && VARIANT[m->variant].volley > 1 ? ((m->volley_left % 3) - 1) * 0.09f : 0;
    float ang = atan2f(dy, dx) + spread + (frand(r) - 0.5f) * 0.04f;
    const float V = r->difficulty == 2 ? 470 : r->difficulty == 1 ? 410 : 350;
    s->vx = cosf(ang) * V; s->vy = sinf(ang) * V; s->vz = ((CAM_H - 8) - s->z) / (d / V);
    s->life = d / V + 1.5f; s->on = true; s->enemy = true; s->dmg = VARIANT[m->variant].shot_dmg; s->px = s->x; s->py = s->y; s->pz = s->z;
    spawn_flash(r, s->x, s->y, s->z, 1.2f * m->scale, true);
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
    const float hpmul = r->difficulty == 0 ? 0.8f : r->difficulty == 1 ? 1.0f : 1.25f;
    float a = r->heading + sp->ang, d = 1100 + frand(r) * 250;
    m->variant = sp->variant; m->x = r->px + cosf(a) * d; m->y = r->py + sinf(a) * d;
    m->hp = m->hp_max = VARIANT[sp->variant].hp * hpmul; m->scale = VARIANT[sp->variant].scale;
    m->st = M_ENTER; m->strafe = frand(r) < 0.5f ? -1 : 1; m->pref = 380 + frand(r) * 320; m->anim = frand(r) * 4;
    m->fire_t = 0.5f + frand(r) * 1.0f;
}

static void push_apart(float *ax, float *ay, float bx, float by, float minr)
{
    float dx = *ax - bx, dy = *ay - by, d = hypotf(dx, dy);
    if (d < minr && d > 0.01f) { *ax = bx + dx / d * minr; *ay = by + dy / d * minr; }
}

static void mech_update(Ramrod *r, Mech *m, int idx, float dt)
{
    const float diffk = r->difficulty == 0 ? 0.75f : r->difficulty == 1 ? 1.0f : 1.25f;
    float dx = r->px - m->x, dy = r->py - m->y, d = hypotf(dx, dy); if (d < 1) d = 1;
    float ux = dx / d, uy = dy / d, mvx = 0, mvy = 0, spd = VARIANT[m->variant].speed;
    if (m->flash > 0) m->flash -= dt;
    m->st_t -= dt;
    switch (m->st) {
    case M_ENTER:      /* marching in from the ridge */
        mvx = ux * spd; mvy = uy * spd;
        if (d < 950) { m->st = M_APPROACH; }
        break;
    case M_APPROACH:
        mvx = ux * spd; mvy = uy * spd;
        m->fire_t -= dt * diffk;
        if (d < m->pref) { m->st = M_CIRCLE; m->st_t = 1.0f + frand(r) * 1.5f; }
        else if (m->fire_t <= 0 && attackers(r, false) < 2 + r->difficulty + (m->variant == V_COMMANDER)) { m->st = M_AIM; m->st_t = VARIANT[m->variant].aim / diffk; }   /* opens fire on the way in */
        break;
    case M_CIRCLE: {   /* strafe round the player, holding the preferred range */
        float rad = (d - m->pref) * 0.8f;
        mvx = -uy * m->strafe * spd * 0.8f + ux * clampf(rad, -spd, spd);
        mvy = ux * m->strafe * spd * 0.8f + uy * clampf(rad, -spd, spd);
        m->fire_t -= dt * diffk;
        if (d < 230 && attackers(r, true) == 0) { m->st = M_WINDUP; m->st_t = 0.65f / diffk; m->punch_dir = atan2f(dy, dx); sfx_file("charge.wav"); break; }   /* too close: it swings */
        if (m->st_t <= 0) {
            m->st_t = 0.6f + frand(r) * 1.0f;
            if (frand(r) < 0.3f) m->strafe = -m->strafe;
            int cap = 2 + r->difficulty + (m->variant == V_COMMANDER);   /* the command mech doesn't wait its turn */
            if (d < 700 && frand(r) < 0.45f * diffk && attackers(r, true) == 0 && attackers(r, false) < cap) { m->st = M_CHARGE; m->st_t = 3.0f; }
            else if (m->fire_t <= 0 && d < 1700 && attackers(r, false) < cap) { m->st = M_AIM; m->st_t = VARIANT[m->variant].aim / diffk; if (d < 1000) sfx_file("charge.wav"); }
            else m->pref = 360 + frand(r) * 360;
        }
        break; }
    case M_AIM:
        if (m->st_t <= 0) { m->st = M_FIRE; m->volley_left = VARIANT[m->variant].volley; m->st_t = 0; }
        break;
    case M_FIRE:
        if (m->st_t <= 0) {
            fire_plasma(r, m); m->volley_left--; m->st_t = 0.24f;
            if (m->volley_left <= 0) { m->st = M_CIRCLE; m->st_t = 0.4f + frand(r) * 0.5f; m->fire_t = VARIANT[m->variant].rest * (1 + frand(r)); }
        }
        break;
    case M_CHARGE:     /* runs at the player */
        mvx = ux * spd * 2.1f; mvy = uy * spd * 2.1f;
        if (d < 225) { m->st = M_WINDUP; m->st_t = 0.6f / diffk; m->punch_dir = atan2f(dy, dx); sfx_file("charge.wav"); }
        else if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = 1.0f; }
        break;
    case M_WINDUP:
        if (d > 260) { mvx = ux * spd; mvy = uy * spd; }
        if (m->st_t <= 0) {
            m->st = M_PUNCH; m->st_t = 0.35f;
            sfx_file("whoosh.wav");
            float off = fabsf(remainderf(atan2f(dy, dx) - m->punch_dir, TWO_PI));
            if (d < PUNCH_RANGE * m->scale + 30 && off < 0.3f) { player_hurt(r, VARIANT[m->variant].punch_dmg, 0.6f, "punch"); sfx_file("clang.wav"); r->white = 0.25f; }
        }
        break;
    case M_PUNCH:
        if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = 1.2f; m->pref = 420 + frand(r) * 200; m->fire_t = fmaxf(m->fire_t, 1.0f); }
        break;
    case M_STAGGER:
        if (m->st_t <= 0) { m->st = M_CIRCLE; m->st_t = 0.6f; m->pref = 380 + frand(r) * 200; }
        break;
    case M_DYING: {   /* bursts all over it while it sinks, then the big one */
        m->dying_t += dt; m->expl_t -= dt;
        if (m->expl_t <= 0 && m->dying_t < 1.3f) {
            m->expl_t = 0.11f;
            float rx = -uy, ry = ux, o = (frand(r) - 0.5f) * 60 * m->scale;
            spawn_expl(r, m->x + rx * o - ux * 4, m->y + ry * o - uy * 4, (20 + frand(r) * 90) * m->scale, 0.8f + frand(r) * 0.6f);
            if (frand(r) < 0.5f) sfx_play(5, 0);
        }
        if (m->dying_t >= 1.5f) {
            for (int k = 0; k < 3; k++) spawn_expl(r, m->x + (frand(r) - 0.5f) * 50, m->y + (frand(r) - 0.5f) * 50, 30 + k * 30, 2.2f + frand(r));
            for (int k = 0; k < 10; k++) {
                Fx *e = fx_new(r); if (!e) break;
                e->kind = FX_DEBRIS; e->x = m->x; e->y = m->y; e->z = 50 * m->scale; e->frame = k % 6;
                float a = frand(r) * TWO_PI, v = 80 + frand(r) * 160;
                e->vx = cosf(a) * v; e->vy = sinf(a) * v; e->vz = 180 + frand(r) * 200; e->dur = 3.0f; e->scale = 1.2f + frand(r); e->spin = frand(r) < 0.5f;
            }
            Fx *sc = fx_new(r); if (sc) { sc->kind = FX_SCORCH; sc->x = m->x; sc->y = m->y; sc->scale = 1.6f * m->scale; sc->dur = 1e9f; }
            for (int k = 0; k < 5; k++) spawn_smoke(r, m->x + (frand(r) - 0.5f) * 60, m->y + (frand(r) - 0.5f) * 60, 20, 1.5f);
            sfx_play(6, 0); r->shake = fmaxf(r->shake, d < 600 ? 0.35f : 0.15f);
            m->st = M_OFF;
        }
        return; }
    default: return;
    }
    /* knockback, movement, walking animation */
    m->kvx = approach(m->kvx, 0, 700 * dt); m->kvy = approach(m->kvy, 0, 700 * dt);
    float ox = m->x, oy = m->y;
    m->x += (mvx + m->kvx) * dt; m->y += (mvy + m->kvy) * dt;
    /* keep out of Ramrod, the other mechs and the rocks */
    push_apart(&m->x, &m->y, r->px, r->py, PLAYER_R + MECH_R * m->scale + 60);
    for (int j = 0; j < MAX_MECH; j++) if (j != idx && r->mech[j].st != M_OFF && r->mech[j].st != M_DYING) push_apart(&m->x, &m->y, r->mech[j].x, r->mech[j].y, 110);
    for (int j = 0; j < r->nprop; j++) push_apart(&m->x, &m->y, r->prop[j].x, r->prop[j].y, r->prop[j].r + MECH_R);
    float moved = hypotf(m->x - ox, m->y - oy);
    int before = (int)m->anim;
    m->anim += moved / 24.0f;
    int after = (int)m->anim;
    if (after != before && (after & 1) == 0 && d < 520) sfx_file("stomp.wav");   /* its footfalls when it's close */
}

/* ---------------------------------------------------------------- Ramrod */
static void player_control(Ramrod *r, const Input *in, float dt)
{
    bool strafe = btn_down(in, BTN_AIM);
    float turn = 0;
    if (!strafe) { if (btn_down(in, BTN_LEFT)) turn -= 1; if (btn_down(in, BTN_RIGHT)) turn += 1; }
    r->turn_v = approach(r->turn_v, turn * 1.7f, 7.0f * dt);
    r->heading += r->turn_v * dt;
    float fwd = 0; if (btn_down(in, BTN_UP)) fwd += 1; if (btn_down(in, BTN_DOWN)) fwd -= 1;
    r->speed = approach(r->speed, fwd > 0 ? 135 : fwd < 0 ? -90 : 0, 320 * dt);
    float st = 0; if (strafe) { if (btn_down(in, BTN_LEFT)) st -= 1; if (btn_down(in, BTN_RIGHT)) st += 1; }
    r->strafe_v = approach(r->strafe_v, st * 120, 420 * dt);
    float c = cosf(r->heading), s = sinf(r->heading);
    r->px += (c * r->speed - s * r->strafe_v) * dt; r->py += (s * r->speed + c * r->strafe_v) * dt;
    for (int j = 0; j < r->nprop; j++) push_apart(&r->px, &r->py, r->prop[j].x, r->prop[j].y, r->prop[j].r + PLAYER_R);
    float dc = hypotf(r->px, r->py);
    if (dc > ARENA_R) {   /* the zone's edge: Ramrod won't leave the outpost behind */
        r->px *= ARENA_R / dc; r->py *= ARENA_R / dc;
        if (r->msg_t <= 0) set_msg(r, "RETURN TO THE OUTPOST", "THE TOWN IS BEHIND YOU", 1.5f);
    }
    /* the stride: the view dips on every footfall */
    float gait = fabsf(r->speed) + fabsf(r->strafe_v) * 0.8f;
    if (gait > 5) {
        float prev = r->step_phase; r->step_phase += gait * dt / 95.0f;
        if ((int)(prev * 2) != (int)(r->step_phase * 2)) { sfx_file("stomp.wav"); r->shake = fmaxf(r->shake, 0.06f); }
    }
    float target_bob = gait > 5 ? -fabsf(sinf(r->step_phase * TWO_PI)) * 3.0f + 1.5f : 0;
    r->bob = approach(r->bob, target_bob, 30 * dt);

    /* the guns: held fire, alternating shoulders; heat builds up */
    if (r->dlg.active) r->fire_hold = true; else if (!btn_down(in, BTN_SHOOT)) r->fire_hold = false;
    if (r->dlg.active) r->punch_hold = true; else if (!btn_down(in, BTN_JUMP)) r->punch_hold = false;
    r->fire_cd -= dt; r->gun_idle += dt;
    if (btn_down(in, BTN_SHOOT) && !r->fire_hold && !r->overheated && r->fire_cd <= 0) {
        fire_bolt(r); r->fire_cd = 0.13f; r->heat += 0.062f; r->gun_idle = 0;
        if (r->heat >= 1) { r->heat = 1; r->overheated = true; set_msg(r, "GUNS OVERHEATED", NULL, 1.2f); sfx_file("alarm.wav"); }
    }
    r->heat = fmaxf(0, r->heat - dt * (r->gun_idle > 0.35f ? 0.55f : 0.22f));
    if (r->overheated && r->heat < 0.3f) r->overheated = false;

    /* the fists: alternate left / right, the hit lands as the arm reaches out */
    r->punch_cd -= dt;
    if (btn_pressed(in, BTN_JUMP) && !r->punch_hold && r->punch_cd <= 0) {
        r->punch_t = 0; r->punch_side ^= 1; r->punch_hit_done = false; r->punch_cd = 0.5f;
        r->speed += 50; sfx_file("whoosh.wav");
    }
    if (r->punch_t >= 0) {
        r->punch_t += dt;
        if (!r->punch_hit_done && r->punch_t >= 0.19f) {
            r->punch_hit_done = true;
            Mech *best = NULL; float bf = 1e9f;
            for (int i = 0; i < MAX_MECH; i++) {
                Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
                float f, l; to_cam(r, m->x, m->y, &f, &l);
                if (f > 0 && f < PUNCH_RANGE + MECH_R * m->scale && fabsf(l) < 70 + f * 0.25f && f < bf) { bf = f; best = m; }
            }
            if (best) {
                bool counter = best->st == M_WINDUP;
                mech_damage(r, best, counter ? 9 : 6, true);
                if (counter && best->st == M_STAGGER) set_msg(r, "COUNTER!", NULL, 0.8f);
                sfx_file("clang.wav"); sfx_play(14, 0); r->shake = fmaxf(r->shake, 0.2f);
                float c2 = cosf(r->heading), s2 = sinf(r->heading);
                spawn_flash(r, best->x - c2 * 30, best->y - s2 * 30, 70 * best->scale, 2.0f, false);
                spawn_expl(r, best->x - c2 * 30, best->y - s2 * 30, 70 * best->scale, 0.6f);
            }
            for (int i = 0; i < MAX_SHOT; i++) {   /* a fist swats an incoming plasma ball out of the air */
                Shot *s = &r->shot[i]; if (!s->on || !s->enemy) continue;
                float f, l; to_cam(r, s->x, s->y, &f, &l);
                if (f > 0 && f < 200 && fabsf(l) < 80) { s->on = false; spawn_expl(r, s->x, s->y, s->z, 0.7f); set_msg(r, "PARRY!", NULL, 0.7f); }
            }
        }
        if (r->punch_t > 0.56f) r->punch_t = -1;
    }
}

static void update_lock(Ramrod *r, float dt)
{
    int best = -1; float ba = 0.13f;
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
        float f, l; to_cam(r, m->x, m->y, &f, &l);
        if (f < 40 || f > 1600) continue;
        float a = fabsf(atan2f(l, f)); if (a < ba) { ba = a; best = i; }
    }
    if (best != r->lock) { r->lock = best; r->lock_t = 0; if (best >= 0) sfx_play(0, 0); }
    r->lock_t += dt;
}

static void update_shots(Ramrod *r, float dt)
{
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *s = &r->shot[i]; if (!s->on) continue;
        s->px = s->x; s->py = s->y; s->pz = s->z;
        s->x += s->vx * dt; s->y += s->vy * dt; s->z += s->vz * dt; s->life -= dt;
        if (s->life <= 0 || s->z < 0) { if (s->z < 0) spawn_expl(r, s->x, s->y, 4, 0.4f); s->on = false; continue; }
        if (!s->enemy) {
            for (int j = 0; j < MAX_MECH && s->on; j++) {
                Mech *m = &r->mech[j]; if (m->st == M_OFF || m->st == M_DYING) continue;
                if (hypotf(s->x - m->x, s->y - m->y) < MECH_R * m->scale + 6 && s->z < MECH_H * m->scale) {
                    s->on = false; mech_damage(r, m, 1, false); sfx_play(14, 0);
                    spawn_flash(r, s->x, s->y, s->z, 0.9f, false);
                }
            }
            for (int j = 0; j < MAX_SHOT && s->on; j++) {   /* bolts shoot plasma down */
                Shot *o = &r->shot[j]; if (!o->on || !o->enemy) continue;
                float dx = s->x - o->x, dy = s->y - o->y, dz = s->z - o->z;
                if (dx * dx + dy * dy + dz * dz < 16 * 16) { s->on = false; o->on = false; spawn_expl(r, o->x, o->y, o->z, 0.6f); sfx_play(13, 0); }
            }
            for (int j = 0; j < r->nprop && s->on; j++) {
                Prop *p = &r->prop[j];
                if (hypotf(s->x - p->x, s->y - p->y) < p->r && s->z < 40 * p->scale) { s->on = false; spawn_flash(r, s->x, s->y, s->z, 0.7f, false); }
            }
        } else {
            float dx = s->x - r->px, dy = s->y - r->py;
            if (dx * dx + dy * dy < 48 * 48) {
                s->on = false; spawn_flash(r, s->x, s->y, s->z, 2.5f, true);
                player_hurt(r, s->dmg, 0.35f, "plasma");
            }
        }
    }
}

static void update_fx(Ramrod *r, float dt)
{
    for (int i = 0; i < MAX_FX; i++) {
        Fx *e = &r->fx[i]; if (!e->on) continue;
        e->t += dt;
        if (e->kind == FX_DEBRIS) {
            e->vz -= 520 * dt; e->x += e->vx * dt; e->y += e->vy * dt; e->z += e->vz * dt;
            if (e->z < 0) { e->z = 0; e->vz = -e->vz * 0.3f; e->vx *= 0.6f; e->vy *= 0.6f; }
        } else if (e->kind == FX_SMOKE) { e->z += e->vz * dt; e->scale += dt * 0.8f; }
        if (e->t >= e->dur) e->on = false;
    }
}

static int alive_mechs(const Ramrod *r) { int n = 0; for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) n++; return n; }

/* debug (SABER_R6BOT=1): a crude autopilot for balance / flow tests - faces the nearest mech, fires while locked
 * (minding the heat), punches up close, sidesteps plasma */
static void bot_input(Ramrod *r, Input *out)
{
    for (int b = 0; b < BTN_COUNT; b++) out->state[b] = 1;
    Mech *t = NULL; float td = 1e9f;
    for (int i = 0; i < MAX_MECH; i++) { Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue; float d = hypotf(m->x - r->px, m->y - r->py); if (d < td) { td = d; t = m; } }
    static int ppress; ppress++;
    if (t) {
        float f, l; to_cam(r, t->x, t->y, &f, &l); float a = atan2f(l, f);
        if (a > 0.04f) out->state[BTN_RIGHT] = 0; else if (a < -0.04f) out->state[BTN_LEFT] = 0;
        if (fabsf(a) < 0.15f && r->heat < 0.85f && !r->overheated) out->state[BTN_SHOOT] = (ppress & 1) ? 0 : 0;
        if (td > 600) out->state[BTN_UP] = 0;
        if (td < 280 && fabsf(a) < 0.3f && (ppress % 20) == 0) out->state[BTN_JUMP] = 2;
    }
    for (int i = 0; i < MAX_SHOT; i++) {
        Shot *s = &r->shot[i]; if (!s->on || !s->enemy) continue;
        float f, l; to_cam(r, s->x, s->y, &f, &l);
        if (f > 0 && f < 400 && fabsf(l) < 70) { out->state[BTN_AIM] = 0; out->state[BTN_LEFT] = 1; out->state[BTN_RIGHT] = 1; out->state[l > 0 ? BTN_LEFT : BTN_RIGHT] = 0; break; }
    }
}

static void update_world(Ramrod *r, const Input *in, float dt, bool control)
{
    Input bot;
    if (control && SDL_getenv("SABER_R6BOT")) { bot_input(r, &bot); in = &bot; }
    if (control) player_control(r, in, dt);
    else { r->speed = approach(r->speed, 0, 300 * dt); r->strafe_v = approach(r->strafe_v, 0, 400 * dt); r->turn_v = approach(r->turn_v, 0, 7 * dt); r->bob = approach(r->bob, 0, 20 * dt); if (r->punch_t >= 0) { r->punch_t += dt; if (r->punch_t > 0.56f) r->punch_t = -1; } }
    for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) mech_update(r, &r->mech[i], i, dt);
    update_shots(r, dt);
    update_fx(r, dt);
    update_lock(r, dt);
}

/* ---------------------------------------------------------------- flow */
void ramrod_update(Ramrod *r, const Input *in, float dt)
{
    if (!r->ok) { r->result = 1; return; }
    if (r->result) return;
    if (btn_pressed(in, BTN_PAUSE) && fighting(r)) { r->paused = !r->paused; sfx_play(10, 0); music_pause(r->paused); }
    if (r->paused) return;
    r->phase_t += dt; r->total_t += dt;
    if (r->msg_t > 0) r->msg_t -= dt;
    if (r->hurt_t > 0) r->hurt_t -= dt;
    if (r->shake > 0) r->shake -= dt;
    r->red = fmaxf(0, r->red - dt * 1.6f); r->white = fmaxf(0, r->white - dt * 1.5f);
    { static int last = -1; if (SDL_getenv("SABER_TRACE") && r->phase != last) { fprintf(stderr, "r6 phase %d wave %d t=%.1f armor=%.0f\n", r->phase, r->wave, r->total_t, r->armor); last = r->phase; } }
    if (r->dlg_pending) { r->dlg_pending = false; dialog_open_script(&r->dlg, r->pending_script); }

    switch (r->phase) {
    case PH_INTRO:
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { r->phase = PH_STRIDE; r->phase_t = 0; r->white = 1.0f; sfx_play(0x13, 0); r->shake = 0.6f; }
        update_fx(r, dt);
        break;
    case PH_STRIDE:   /* the transformation: a white flash, the cockpit shudders, POWER STRIDE */
        if (r->phase_t < 0.8f) r->white = fmaxf(r->white, 1.0f - r->phase_t / 0.8f);
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
        while (r->spawned < w->n && r->wave_t >= w->s[r->spawned].delay + 1.0f && active < w->max_active) { spawn_mech(r, &w->s[r->spawned]); r->spawned++; active++; }
        if (r->spawned < w->n && active == 0 && r->wave_t > 1.0f) r->wave_t = fmaxf(r->wave_t, w->s[r->spawned].delay + 1.0f);   /* nobody left: bring the next one in now */
        update_world(r, in, dt, true);
        if (SDL_getenv("SABER_TRACE") && (int)r->total_t != (int)(r->total_t - dt)) fprintf(stderr, "r6 t=%.0f wave=%d spawned=%d alive=%d armor=%.0f heat=%.2f pos=%.0f,%.0f h=%.2f kills=%d\n", r->total_t, r->wave, r->spawned, alive_mechs(r), r->armor, r->heat, r->px, r->py, r->heading, r->kills);
        if (r->phase != PH_DOWN && r->spawned >= w->n && alive_mechs(r) == 0) {
            r->phase = PH_WAVE_CLEAR; r->phase_t = 0;
            set_msg(r, r->wave == N_WAVES - 1 ? "SQUADRON DESTROYED" : "WAVE CLEARED", NULL, 2.2f);
            for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].enemy) r->shot[i].on = false;
            if (r->wave == N_WAVES - 1) { music_play(6, false); r->music_now = 6; }
        }
        if (r->armor > 0 && r->armor < 30) { r->alarm_t -= dt; if (r->alarm_t <= 0) { r->alarm_t = 1.2f; sfx_file("alarm.wav"); } }
        break; }
    case PH_WAVE_CLEAR:
        update_world(r, in, dt, false);
        if (r->phase_t >= 2.4f) {
            if (r->wave == N_WAVES - 1) { r->phase = PH_OUTRO; r->phase_t = 0; dialog_open_script(&r->dlg, SCRIPT_OUTRO); }
            else {
                r->phase = PH_RADIO; r->phase_t = 0; dialog_open_script(&r->dlg, *SCRIPT_BETWEEN[r->wave]);
                float heal = r->difficulty == 2 ? 25 : 40;   /* April patches the armour between waves; before the */
                if (r->wave == N_WAVES - 2 && r->difficulty < 2) heal = r->armor_max;   /* squadron she reroutes everything */
                r->armor = fminf(r->armor_max, r->armor + heal);
            }
        }
        break;
    case PH_RADIO:
        update_fx(r, dt);
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { begin_wave(r, r->wave + 1); r->fire_hold = r->punch_hold = true; }
        break;
    case PH_DOWN:     /* Ramrod goes down: the cockpit shakes, sparks, the screen goes dark */
        r->shake = 0.3f; r->red = fmaxf(r->red, 0.4f + 0.3f * sinf(r->phase_t * 12));
        if (frand(r) < dt * 6) { float a = r->heading + (frand(r) - 0.5f) * 1.0f; spawn_expl(r, r->px + cosf(a) * 120, r->py + sinf(a) * 120, 20 + frand(r) * 60, 1.0f); }
        update_world(r, in, dt, false);
        r->black = clampf((r->phase_t - 1.8f) / 1.0f, 0, 1);
        if (r->phase_t >= 3.2f) {
            if (r->lives <= 0) { r->phase = PH_GAMEOVER; r->phase_t = 0; }
            else {   /* a spare: Ramrod is back on its feet, the wave starts over */
                r->lives--; r->armor = r->armor_max; r->heat = 0; r->overheated = false; r->black = 0; r->red = 0;
                begin_wave(r, r->wave);
            }
        }
        break;
    case PH_GAMEOVER:
        r->black = 1;
        if (r->phase_t > 1.0f) r->result = 2;
        break;
    case PH_OUTRO:
        update_fx(r, dt);
        if (r->dlg.active) dialog_update(&r->dlg, in, dt);
        else { r->phase = PH_CLEARED; r->phase_t = 0; }
        break;
    case PH_CLEARED:
        if (r->phase_t > 1.2f) r->result = 1;
        break;
    }
}

/* ---------------------------------------------------------------- drawing */
static void draw_frame(Ramrod *r, int a, int fr, float x, float y, float scale, bool flip, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    Anim *an = &r->anim[a]; if (an->n == 0) return;
    fr = fr < 0 ? 0 : fr >= an->n ? an->n - 1 : fr;
    Frame *f = &an->f[fr];
    SDL_FRect src = { (float)f->x, (float)f->y, (float)f->w, (float)f->h };
    float ax = flip ? f->w - 1 - f->ax : f->ax;
    SDL_FRect dst = { floorf(x - ax * scale), floorf(y - f->ay * scale), roundf(f->w * scale), roundf(f->h * scale) };
    if (dst.w < 1 || dst.h < 1) return;
    SDL_SetTextureColorMod(r->atlas, cr, cg, cb); SDL_SetTextureAlphaMod(r->atlas, ca);
    SDL_RenderTextureRotated(r->ren, r->atlas, &src, &dst, 0, NULL, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureColorMod(r->atlas, 255, 255, 255); SDL_SetTextureAlphaMod(r->atlas, 255);
}

static void fill_ellipse(SDL_Renderer *ren, float cx, float cy, float rx, float ry, SDL_FColor c)
{
    enum { N = 20 }; SDL_Vertex v[N + 1]; int idx[N * 3];
    v[0].position = (SDL_FPoint){ cx, cy }; v[0].color = c;
    for (int i = 0; i < N; i++) { float a = i * TWO_PI / N; v[i + 1].position = (SDL_FPoint){ cx + cosf(a) * rx, cy + sinf(a) * ry }; v[i + 1].color = c; }
    for (int i = 0; i < N; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = 1 + i; idx[i * 3 + 2] = 1 + (i + 1) % N; }
    SDL_RenderGeometry(ren, NULL, v, N + 1, idx, N * 3);
}

static void render_sky(Ramrod *r)
{
    float u = PANO_AHEAD + (r->heading - HEADING0) * FOCAL - r->sw * 0.5f;
    u = fmodf(u, (float)PANO_W); if (u < 0) u += PANO_W;
    float y0 = r->bob + (HZ - 167.0f);
    SDL_SetRenderDrawColor(r->ren, 35, 131, 201, 255); SDL_FRect top = { 0, 0, (float)r->sw, fmaxf(0, y0) + 1 }; SDL_RenderFillRect(r->ren, &top);
    for (float x = -u; x < r->sw; x += PANO_W) { SDL_FRect dst = { floorf(x), floorf(y0), PANO_W, PANO_H }; SDL_RenderTexture(r->ren, r->sky, NULL, &dst); }
}

static void render_floor(Ramrod *r)
{
    float c = cosf(r->heading), s = sinf(r->heading), rx = -s, ry = c;
    int sw = r->sw; float hz = horizon(r);
    int y0 = (int)ceilf(hz + 0.01f);
    const uint32_t haze = 0xFF7AAAD9u;   /* ABGR: warm dust at the horizon (d9 aa 7a) */
    for (int row = 0; row < r->floor_h; row++) {
        int y = y0 + row; if (y >= r->sh) break;
        float d = CAM_H * FOCAL / (y + 0.5f - hz);
        float step = d / FOCAL;
        float fog = clampf((d - FOG0) / (FOG1 - FOG0), 0, 1); int fa = (int)(fog * 230);
        int mip = step < 1.4f ? 0 : step < 2.8f ? 1 : step < 5.6f ? 2 : 3;
        int msz = TEX >> mip, mmask = msz - 1, msh = 8 - mip;
        float wx = r->px + c * d - rx * step * (sw * 0.5f), wy = r->py + s * d - ry * step * (sw * 0.5f);
        uint32_t *out = r->floor_px + (size_t)row * sw;
        for (int x = 0; x < sw; x++) {
            int ix = (int)floorf(wx) >> mip, iy = (int)floorf(wy) >> mip;
            uint32_t col = r->tex[mip][((iy & mmask) << msh) + (ix & mmask)];
            if (fa) {
                uint32_t R = ((col & 0xff) * (256 - fa) + (haze & 0xff) * fa) >> 8;
                uint32_t G = (((col >> 8) & 0xff) * (256 - fa) + ((haze >> 8) & 0xff) * fa) >> 8;
                uint32_t B = (((col >> 16) & 0xff) * (256 - fa) + ((haze >> 16) & 0xff) * fa) >> 8;
                col = 0xff000000u | B << 16 | G << 8 | R;
            }
            out[x] = col; wx += rx * step; wy += ry * step;
        }
    }
    SDL_UpdateTexture(r->floor_tex, NULL, r->floor_px, sw * 4);
    SDL_FRect dst = { 0, (float)y0, (float)sw, (float)r->floor_h };
    SDL_RenderTexture(r->ren, r->floor_tex, NULL, &dst);
}

typedef struct { float f; int kind, i; } Item;
static int cmp_far(const void *a, const void *b) { float x = ((const Item *)a)->f, y = ((const Item *)b)->f; return x < y ? 1 : x > y ? -1 : 0; }

static uint8_t fog_alpha(float f) { return (uint8_t)(255 * (1 - clampf((f - 2000) / 900, 0, 1))); }

static void draw_mech(Ramrod *r, Mech *m, float f)
{
    float sx, sy, k; if (!project(r, m->x, m->y, 0, &sx, &sy, &k)) return;
    float sc = k * UNIT * m->scale;
    int fr = MF_WALK0 + ((int)m->anim & 3);
    switch (m->st) {
    case M_AIM: case M_FIRE: fr = MF_AIM; break;
    case M_WINDUP: fr = MF_WINDUP; break;
    case M_PUNCH: fr = MF_PUNCH; break;
    case M_STAGGER: fr = MF_STAGGER; break;
    default: break;
    }
    /* its shadow on the sand */
    SDL_SetRenderDrawBlendMode(r->ren, SDL_BLENDMODE_BLEND);
    fill_ellipse(r->ren, sx, sy, 44 * sc, 44 * sc * clampf(CAM_H / f * 1.4f, 0.08f, 0.5f), (SDL_FColor){ 0.2f, 0.1f, 0.08f, 0.35f });
    uint8_t cr = 255, cg = 255, cb = 255, ca = fog_alpha(f);
    if (m->flash > 0) { cr = 255; cg = 255; cb = 255; }
    if (m->st == M_DYING) {   /* burning, sinking into its own blast */
        float t = m->dying_t / 1.5f; uint8_t v = (uint8_t)(255 - 150 * t);
        cr = v; cg = (uint8_t)(v * 0.7f); cb = (uint8_t)(v * 0.6f);
        if (((int)(m->dying_t * 20)) & 1) { cr = 255; cg = 200; cb = 150; }
        SDL_Rect clip = { 0, 0, r->sw, (int)sy }; SDL_SetRenderClipRect(r->ren, &clip);
        draw_frame(r, VARIANT[m->variant].anim, MF_STAGGER, sx + sinf(m->dying_t * 40) * 1.5f, sy + t * t * 60 * sc, sc, false, cr, cg, cb, ca);
        SDL_SetRenderClipRect(r->ren, NULL);
        return;
    }
    draw_frame(r, VARIANT[m->variant].anim, fr, sx, sy, sc, false, cr, cg, cb, ca);
    if (m->flash > 0) {   /* hit flash: the sprite again, additive */
        SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_ADD);
        draw_frame(r, VARIANT[m->variant].anim, fr, sx, sy, sc, false, 255, 255, 255, 200);
        SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_BLEND);
    }
    /* the cannon charging / the fist glowing red before a punch: the telegraphs */
    if (m->st == M_AIM || m->st == M_WINDUP) {
        bool aim = m->st == M_AIM;
        float fx = sx + (aim ? 22 : 30) * sc, fy = sy - (aim ? 92 : 84) * sc;
        float p = aim ? 1 - clampf(m->st_t / 0.75f, 0, 1) : 1 - clampf(m->st_t / 0.6f, 0, 1);
        SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_ADD);
        if (aim) draw_frame(r, A_PLASMA, (int)(r->total_t * 16) & 3, fx, fy, sc * (0.4f + 0.9f * p), false, 255, 255, 255, 255);
        else draw_frame(r, A_MFLASH, (int)(r->total_t * 20) & 1, fx, fy, sc * (0.8f + 1.4f * p), false, 255, 60, 40, 255);
        SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_BLEND);
    }
}

static void render_world(Ramrod *r)
{
    Item items[MAX_MECH + MAX_SHOT + MAX_FX + MAX_PROP]; int n = 0;
    for (int i = 0; i < r->nprop; i++) { float f, l; to_cam(r, r->prop[i].x, r->prop[i].y, &f, &l); if (f > 10 && f < 3200) items[n++] = (Item){ f, 0, i }; }
    for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st != M_OFF) { float f, l; to_cam(r, r->mech[i].x, r->mech[i].y, &f, &l); if (f > 20) items[n++] = (Item){ f, 1, i }; }
    for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].on) { float f, l; to_cam(r, r->shot[i].x, r->shot[i].y, &f, &l); if (f > 10) items[n++] = (Item){ f, 2, i }; }
    for (int i = 0; i < MAX_FX; i++) if (r->fx[i].on) { float f, l; to_cam(r, r->fx[i].x, r->fx[i].y, &f, &l); if (f > 10) items[n++] = (Item){ r->fx[i].kind == FX_SCORCH ? f + 1e5f : f, 3, i }; }
    qsort(items, n, sizeof *items, cmp_far);
    for (int k = 0; k < n; k++) {
        Item *it = &items[k]; float sx, sy, kk;
        switch (it->kind) {
        case 0: { Prop *p = &r->prop[it->i]; if (!project(r, p->x, p->y, 0, &sx, &sy, &kk)) break;
            draw_frame(r, p->anim, 0, sx, sy, kk * UNIT * p->scale, p->flip, 255, 255, 255, fog_alpha(it->f)); break; }
        case 1: draw_mech(r, &r->mech[it->i], it->f); break;
        case 2: { Shot *s = &r->shot[it->i]; if (!project(r, s->x, s->y, s->z, &sx, &sy, &kk)) break;
            float qx, qy, qk;
            SDL_SetRenderDrawBlendMode(r->ren, SDL_BLENDMODE_ADD);
            if (!s->enemy && project(r, s->x - s->vx * 0.035f, s->y - s->vy * 0.035f, s->z - s->vz * 0.035f, &qx, &qy, &qk)) {   /* the bolt's streak */
                SDL_SetRenderDrawColor(r->ren, 255, 140, 30, 255); SDL_RenderLine(r->ren, qx, qy + 1, sx, sy + 1); SDL_RenderLine(r->ren, qx + 1, qy, sx + 1, sy);
                SDL_SetRenderDrawColor(r->ren, 255, 250, 200, 255); SDL_RenderLine(r->ren, qx, qy, sx, sy);
            }
            SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_ADD);
            if (s->enemy) draw_frame(r, A_PLASMA, (int)(r->total_t * 14) & 3, sx, sy, fmaxf(0.25f, kk * 1.6f), false, 255, 255, 255, 255);
            else draw_frame(r, A_BOLT, (int)(r->total_t * 20) & 1, sx, sy, fmaxf(0.3f, kk * 1.3f), false, 255, 255, 255, 255);
            SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_BLEND);
            break; }
        case 3: { Fx *e = &r->fx[it->i]; if (!project(r, e->x, e->y, e->z, &sx, &sy, &kk)) break;
            float p = e->t / e->dur;
            switch (e->kind) {
            case FX_EXPL: draw_frame(r, A_EXPL, (int)(p * 6), sx, sy, kk * UNIT * 1.4f * e->scale, e->flip, 255, 255, 255, 255); break;
            case FX_SMOKE: draw_frame(r, A_SMOKE, (int)(p * 4), sx, sy, kk * UNIT * 2.0f * e->scale, false, 255, 255, 255, (uint8_t)(200 * (1 - p))); break;
            case FX_DEBRIS: draw_frame(r, A_DEBRIS, e->frame, sx, sy, kk * UNIT * e->scale, e->spin && ((int)(e->t * 8) & 1), 200, 200, 200, (uint8_t)(255 * (1 - clampf((p - 0.7f) / 0.3f, 0, 1)))); break;
            case FX_FLASH: SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_ADD);
                draw_frame(r, e->frame ? A_MFLASH : A_FLASH, 0, sx, sy, fmaxf(0.3f, kk * UNIT * e->scale), false, 255, 255, 255, 255);
                SDL_SetTextureBlendMode(r->atlas, SDL_BLENDMODE_BLEND); break;
            case FX_SCORCH: draw_frame(r, A_SCORCH, 0, sx, sy + 12 * kk * UNIT * e->scale * clampf(CAM_H / it->f * 1.4f, 0.08f, 0.5f), kk * UNIT * e->scale, false, 255, 255, 255, 100); break;
            }
            break; }
        }
    }
}

/* the reticle, lock brackets and the edge-of-screen threat markers */
static void render_aim(Ramrod *r)
{
    SDL_Renderer *ren = r->ren; float cx = r->sw * 0.5f, cy = AIM_Y + r->bob * 0.5f;
    bool locked = r->lock >= 0;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (locked) {
        Mech *m = &r->mech[r->lock]; float sx, sy, k;
        if (project(r, m->x, m->y, 0, &sx, &sy, &k)) {
            float sc = k * UNIT * m->scale, hw = 40 * sc + 3, top = sy - 140 * sc, bot = sy - 6 * sc;
            float L = clampf(hw * 0.5f, 3, 10), shrink = fmaxf(0, 1 - r->lock_t * 5) * 12;
            float x0 = floorf(sx - hw - shrink), x1 = floorf(sx + hw + shrink), y0 = floorf(top - shrink), y1 = floorf(bot + shrink);
            SDL_SetRenderDrawColor(ren, 255, 60, 60, 255);
            SDL_FRect q[8] = { { x0, y0, L, 1 }, { x0, y0, 1, L }, { x1 - L, y0, L, 1 }, { x1, y0, 1, L }, { x0, y1, L, 1 }, { x0, y1 - L, 1, L }, { x1 - L, y1, L, 1 }, { x1, y1 - L, 1, L } };
            SDL_RenderFillRects(ren, q, 8);
            /* its armour, over the brackets */
            float f = clampf(m->hp / m->hp_max, 0, 1), w = fmaxf(16, x1 - x0);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 160); SDL_FRect bg = { x0, y0 - 5, w, 3 }; SDL_RenderFillRect(ren, &bg);
            SDL_SetRenderDrawColor(ren, 255, 90, 60, 255); SDL_FRect fg = { x0, y0 - 5, w * f, 3 }; SDL_RenderFillRect(ren, &fg);
        }
    }
    uint8_t R = locked ? 255 : 120, G = locked ? 70 : 255, B = locked ? 60 : 140;
    SDL_SetRenderDrawColor(ren, R, G, B, 230);
    float g = locked ? 3 : 5;
    SDL_FRect q[4] = { { cx - g - 6, cy, 6, 1 }, { cx + g + 1, cy, 6, 1 }, { cx, cy - g - 6, 1, 6 }, { cx, cy + g + 1, 1, 6 } };
    SDL_RenderFillRects(ren, q, 4);
    SDL_FRect dot = { cx, cy, 1, 1 }; SDL_RenderFillRect(ren, &dot);
    /* threats outside the view: a chevron on that side, blinking when it is about to fire or swing */
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF || m->st == M_DYING) continue;
        float f, l; to_cam(r, m->x, m->y, &f, &l);
        float a = atan2f(l, f);
        if (fabsf(a) < 0.82f) continue;
        bool danger = m->st == M_AIM || m->st == M_WINDUP || m->st == M_CHARGE;
        if (danger && ((int)(r->total_t * 8) & 1)) continue;
        float x = a > 0 ? r->sw - 12.0f : 6.0f, y = 120 + clampf(fabsf(a) - 0.8f, 0, 2.4f) * 10;
        SDL_SetRenderDrawColor(ren, 255, danger ? 60 : 200, 60, 255);
        for (int k = 0; k < 5; k++) {   /* a solid arrowhead pointing out of the screen */
            float w = (float)(5 - k); SDL_FRect c = { a < 0 ? x + k : x + 5 - k, y - w, 1, w * 2 }; SDL_RenderFillRect(ren, &c);
        }
    }
}

/* Ramrod's arm reaching out past the windshield: the clip's frames played out, held, and pulled back; the right
 * fist is the left one mirrored */
static void render_arm(Ramrod *r)
{
    if (r->punch_t < 0) return;
    float t = r->punch_t; int fr;
    if (t < 0.28f) fr = (int)(t / 0.28f * 10); else if (t < 0.36f) fr = 9; else fr = 9 - (int)((t - 0.36f) / 0.2f * 10);
    if (fr < 0) return;
    if (fr > 9) fr = 9;
    Frame *f = &r->anim[A_ARM].f[fr];
    float x = xoff(r) - f->ax, y = -f->ay + r->bob * 0.3f;
    bool right = r->punch_side == 1;
    if (right) x = r->sw - (x + f->w);
    SDL_FRect src = { (float)f->x, (float)f->y, (float)f->w, (float)f->h }, dst = { floorf(x), floorf(y), (float)f->w, (float)f->h };
    SDL_RenderTextureRotated(r->ren, r->atlas, &src, &dst, 0, NULL, right ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
}

static void bar(SDL_Renderer *ren, float x, float y, float w, float h, float f, uint8_t R, uint8_t G, uint8_t B)
{
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 170); SDL_FRect bg = { x, y, w, h }; SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, R, G, B, 255); SDL_FRect fg = { x, y, floorf(w * clampf(f, 0, 1)), h }; SDL_RenderFillRect(ren, &fg);
}

/* the two monitors hanging from the canopy: radar left, Ramrod's status right */
static void render_monitors(Ramrod *r)
{
    SDL_Renderer *ren = r->ren; float ox = xoff(r);
    Font *small = font_get(0x12072E60);
    /* radar: forward is up, 1 px = 60 units */
    SDL_FRect scr = { ox + 118, 23, 52, 33 };
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 6, 34, 20, 255); SDL_RenderFillRect(ren, &scr);
    float cx = scr.x + scr.w * 0.5f, cy = scr.y + scr.h * 0.5f + 2;
    SDL_SetRenderDrawColor(ren, 20, 90, 50, 255);
    for (int k = 1; k <= 2; k++) for (int i = 0; i < 40; i++) { float a = i * TWO_PI / 40; SDL_RenderPoint(ren, cx + cosf(a) * 8 * k, cy + sinf(a) * 8 * k); }
    SDL_RenderLine(ren, cx, cy, cx - 12, cy - 14); SDL_RenderLine(ren, cx, cy, cx + 12, cy - 14);   /* the view cone */
    float sweep = r->total_t * 3.0f;
    SDL_SetRenderDrawColor(ren, 60, 200, 110, 255); SDL_RenderLine(ren, cx, cy, cx + sinf(sweep) * 16, cy - cosf(sweep) * 16);
    for (int i = 0; i < MAX_MECH; i++) {
        Mech *m = &r->mech[i]; if (m->st == M_OFF) continue;
        float f, l; to_cam(r, m->x, m->y, &f, &l);
        float bx = clampf(cx + l / 60, scr.x + 1, scr.x + scr.w - 3), by = clampf(cy - f / 60, scr.y + 1, scr.y + scr.h - 3);
        bool blink = (m->st == M_AIM || m->st == M_WINDUP || m->st == M_CHARGE) && ((int)(r->total_t * 8) & 1);
        if (m->st == M_DYING) SDL_SetRenderDrawColor(ren, 90, 90, 90, 255);
        else if (m->variant == V_COMMANDER) SDL_SetRenderDrawColor(ren, 255, 210, 40, 255);
        else if (m->variant == V_HEAVY) SDL_SetRenderDrawColor(ren, 255, 70, 60, 255);
        else SDL_SetRenderDrawColor(ren, 120, 255, 200, 255);
        if (blink) SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_FRect b = { floorf(bx), floorf(by), m->variant == V_COMMANDER ? 3.0f : 2.0f, m->variant == V_COMMANDER ? 3.0f : 2.0f }; SDL_RenderFillRect(ren, &b);
    }
    SDL_SetRenderDrawColor(ren, 255, 110, 230, 255);
    for (int i = 0; i < MAX_SHOT; i++) if (r->shot[i].on && r->shot[i].enemy) { float f, l; to_cam(r, r->shot[i].x, r->shot[i].y, &f, &l); float bx = cx + l / 60, by = cy - f / 60; if (bx > scr.x && bx < scr.x + scr.w && by > scr.y && by < scr.y + scr.h) SDL_RenderPoint(ren, bx, by); }
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255); SDL_FRect me = { cx - 1, cy - 1, 2, 2 }; SDL_RenderFillRect(ren, &me);
    /* status */
    SDL_FRect st = { ox + 259, 23, 52, 33 };
    SDL_SetRenderDrawColor(ren, 18, 22, 60, 255); SDL_RenderFillRect(ren, &st);
    float ar = r->armor / r->armor_max;
    bool blink = ar < 0.3f && ((int)(r->total_t * 4) & 1);
    if (small) {
        font_draw(small, "ARM", st.x + 2, st.y + 1, 255, blink ? 80 : 210, blink ? 80 : 120);
        font_draw(small, r->overheated ? "HOT" : "GUN", st.x + 2, st.y + 11, 255, r->overheated ? 80 : 210, r->overheated ? 60 : 120);
    }
    bar(ren, st.x + 24, st.y + 3, 26, 5, ar, ar > 0.5f ? 90 : ar > 0.25f ? 240 : 250, ar > 0.5f ? 220 : ar > 0.25f ? 190 : 60, 60);
    if (r->hurt_t > 0 && ((int)(r->hurt_t * 20) & 1)) bar(ren, st.x + 24, st.y + 3, 26, 5, 1, 255, 255, 255);
    bar(ren, st.x + 24, st.y + 13, 26, 5, r->heat, r->overheated ? 255 : 255, r->overheated ? 60 : 160 - (uint8_t)(100 * r->heat), 40);
    if (small) {
        char buf[32]; int left = WAVES[r->wave].n - r->spawned + alive_mechs(r);
        for (int i = 0; i < MAX_MECH; i++) if (r->mech[i].st == M_DYING) left--;
        snprintf(buf, sizeof buf, "W%d", r->wave + 1); font_draw(small, buf, st.x + 2, st.y + 22, 150, 200, 255);
        snprintf(buf, sizeof buf, "%d", left < 0 ? 0 : left); font_draw(small, buf, st.x + 20, st.y + 22, 255, 120, 120);
        snprintf(buf, sizeof buf, "x%d", r->lives); font_draw(small, buf, st.x + 50 - font_text_width(small, buf), st.y + 22, 255, 255, 255);
    }
    /* static on the screens when hit */
    if (r->hurt_t > 0.25f) {
        for (int i = 0; i < 90; i++) {
            SDL_FRect *q = i & 1 ? &scr : &st; uint8_t v = (uint8_t)(frand(r) * 255);
            SDL_SetRenderDrawColor(ren, v, v, v, 255); SDL_RenderPoint(ren, q->x + frand(r) * q->w, q->y + frand(r) * q->h);
        }
    }
}

static void render_instructions(Ramrod *r, Font *f, Font *small)
{
    float t = r->phase_t; int sw = r->sw, sh = r->sh;
    float open = clampf(t / INSTR_OPEN, 0, 1); open = 1 - (1 - open) * (1 - open);
    SDL_SetRenderDrawBlendMode(r->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r->ren, 0, 0, 0, (uint8_t)(140 * open)); SDL_FRect scrim = { 0, 0, (float)sw, (float)sh }; SDL_RenderFillRect(r->ren, &scrim);
    static const struct { const char *label, *desc; } LINES[] = {
        { "TURN", "Left / Right" }, { "WALK", "Up / Down" }, { "SIDESTEP", "Aim button + Left / Right" },
        { "GUNS", "Shoot button (they overheat)" }, { "PUNCH", "Jump button - close range" },
    };
    int nl = (int)(sizeof LINES / sizeof *LINES);
    float full_h = 34 + nl * 14 + 22, pw = (float)sw - 40, ph = full_h * open, x0 = 20, y0 = (sh - full_h) * 0.5f + (full_h - ph) * 0.5f;
    SDL_SetRenderDrawColor(r->ren, 10, 18, 44, (uint8_t)(255 * open)); SDL_FRect panel = { x0, y0, pw, ph }; SDL_RenderFillRect(r->ren, &panel);
    SDL_SetRenderDrawColor(r->ren, 60, 120, 220, (uint8_t)(255 * open));
    SDL_FRect top = { x0, y0 - 2, pw, 2 }, bot = { x0, y0 + ph, pw, 2 }; SDL_RenderFillRect(r->ren, &top); SDL_RenderFillRect(r->ren, &bot);
    if (open < 1 || !f || !small) return;
    const char *title = "RAMROD - ROBOT MODE";
    font_draw(f, title, x0 + (pw - font_text_width(f, title)) * 0.5f, y0 + 8, 255, 182, 0);
    for (int i = 0; i < nl; i++) {
        font_draw(small, LINES[i].label, x0 + 14, y0 + 30 + i * 14, 255, 224, 192);
        font_draw(small, LINES[i].desc, x0 + (sw < 400 ? 84 : 104), y0 + 30 + i * 14, 220, 230, 255);
    }
    const char *sub = "PUNCH A MECH AS IT WINDS UP TO COUNTER";
    font_draw(small, sub, x0 + (pw - font_text_width(small, sub)) * 0.5f, y0 + full_h - 26, 255, 255, 255);
    if (t >= INSTR_OPEN && ((int)(t * 4) & 1)) { const char *s = "PRESS A BUTTON"; font_draw(small, s, x0 + (pw - font_text_width(small, s)) * 0.5f, y0 + full_h - 12, 200, 200, 200); }
}

static void center_text(Ramrod *r, Font *f, const char *s, float y, uint8_t R, uint8_t G, uint8_t B)
{
    float x = r->sw * 0.5f - font_text_width(f, s) * 0.5f;
    font_draw(f, s, x + 1, y + 1, 0, 0, 0); font_draw(f, s, x, y, R, G, B);
}

void ramrod_draw(Ramrod *r, bool scanlines)
{
    if (!r->ok) return;
    SDL_Renderer *ren = r->ren;
    Font *f = font_get(0x4058897F), *small = font_get(0x12072E60);
    float shx = 0, shy = 0;
    if (r->shake > 0) { shx = (frand(r) - 0.5f) * 6 * fminf(1, r->shake * 3); shy = (frand(r) - 0.5f) * 5 * fminf(1, r->shake * 3); }
    float bob = r->bob; r->bob += shy;   /* the shake moves the world; the cockpit only jolts a little */
    SDL_Rect vp = { (int)shx, 0, r->sw, r->sh }; SDL_SetRenderViewport(ren, &vp);
    render_sky(r);
    render_floor(r);
    render_world(r);
    r->bob = bob;
    SDL_SetRenderViewport(ren, NULL);
    if (fighting(r) || r->phase == PH_WAVE_CLEAR) render_aim(r);
    render_arm(r);
    SDL_FRect cp = { floorf(xoff(r) + shx * 0.3f), floorf(shy * 0.3f), ART_W, 240 };
    SDL_RenderTexture(ren, r->cockpit, NULL, &cp);
    render_monitors(r);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (r->red > 0) { SDL_SetRenderDrawColor(ren, 200, 20, 10, (uint8_t)(110 * clampf(r->red, 0, 1))); SDL_FRect q = { 0, 0, (float)r->sw, (float)r->sh }; SDL_RenderFillRect(ren, &q); }
    if (f && small) {
        if (r->phase == PH_STRIDE) {
            float t = r->phase_t;
            if (t > 0.5f) center_text(r, f, "POWER STRIDE!", 70, 255, 210, 60);
            if (t > 1.1f) center_text(r, small, "RAMROD - ROBOT MODE", 90, 255, 255, 255);
        }
        if (r->phase == PH_WAVE_IN && r->phase_t < WAVE_IN_DUR) {
            const Wave *w = &WAVES[r->wave];
            if ((int)(r->phase_t * 4) & 1 || r->phase_t > 1.2f) center_text(r, f, w->title, 64, 255, 182, 0);
            center_text(r, small, w->sub, 84, 255, 255, 255);
            if (r->wave == N_WAVES - 1 && r->phase_t > 1.0f) center_text(r, small, "WARNING: COMMAND MECH", 96, 255, 80, 80);
        }
        if (r->msg_t > 0 && r->phase != PH_WAVE_IN) { center_text(r, f, r->msg, 70, 255, 255, 255); if (r->msg2[0]) center_text(r, small, r->msg2, 88, 255, 200, 160); }
        if (r->phase == PH_DOWN && r->phase_t > 0.6f) center_text(r, f, "RAMROD IS DOWN!", 70, 255, 60, 60);
        if (r->paused) center_text(r, f, "PAUSE", 100, 255, 255, 255);
    }
    if (r->dlg.active) dialog_draw(&r->dlg, ren, r->sw, r->sh);
    if (r->phase == PH_INSTR) render_instructions(r, f, small);
    if (r->white > 0) { SDL_SetRenderDrawColor(ren, 255, 255, 255, (uint8_t)(255 * clampf(r->white, 0, 1))); SDL_FRect q = { 0, 0, (float)r->sw, (float)r->sh }; SDL_RenderFillRect(ren, &q); }
    if (scanlines) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(ren, 0, 0, 0, 70);
        for (int y = 1; y < r->sh; y += 2) { SDL_FRect q = { 0, (float)y, (float)r->sw, 1 }; SDL_RenderFillRect(ren, &q); }
    }
    float fade = r->phase == PH_CLEARED ? clampf(r->phase_t / 1.2f, 0, 1) : r->black;
    if (fade > 0) {
        uint8_t v = r->phase == PH_CLEARED ? 255 : 0;
        SDL_SetRenderDrawColor(ren, v, v, v, (uint8_t)(255 * fade)); SDL_FRect q = { 0, 0, (float)r->sw, (float)r->sh }; SDL_RenderFillRect(ren, &q);
    }
}
