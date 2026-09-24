#pragma once
#include "platform/render.h"
#include "platform/plat.h"
#include "level.h"
#include "player.h"
#include "input.h"
#include "enemies.h"
#include "dialog.h"
#include "menu.h"
#include "mode7.h"
#include "ramrod.h"
#include "space.h"
#include "night.h"
#include "forest.h"
#include "darkapril.h"
#include "power.h"

typedef struct {
    Ren *ren;
    int sw, sh;
    Level level;
    PhysicsWorld world;
    Player player;
    Bullets player_bullets, enemy_bullets;
    Effects effects;
    Enemies enemies;
    int player_layer;
    Input in;
    real cam_x, cam_y;
    bool cam_locked;
    /* level flow zones (from LEVL objects) */
    struct { real cx, cy, hx, hy; bool armed; } stops[4]; int nstops;        /* 998/999 */
    struct { real cx, cy, hx, hy; bool done; uint32_t text; real focus_x, focus_y, t_in, t_out; } dialogs[4];
    struct { real cx, cy, hx, hy; bool set; } exit_zone;                     /* type 4 */
    struct { real cx, cy, hx, hy, rx, ry; } deathzones[8]; int ndeath;       /* type 3 */
    int state;              /* 10 playing, 0xd dialog, 0xe level clear, 0xb game over, 0xc pause */
    Menu menu; bool in_level;
    int stage;              /* 1 frontier town (LEVL), 2 the Mode-7 Grand Prix, 3 Hyperjumper Pass, 4 the jungle, 5 the cave lab, 6 Ramrod,
                             * 7 stage 6's final phase (the space shooter; "STAGE 6" on screen) */
    int carry_lives;        /* lives left when stage 1 was cleared (-1: fresh start) */
    int continues_left;     /* CONTINUE? credits left in this run (from the option at the start of a run) */
    bool mode7_phase2;      /* a stage-2 continue resumes at the pursuit */
    Mode7 *mode7;
    Ramrod *ramrod;              /* stage 6: Ramrod's cockpit (ramrod.c), its own world like the Grand Prix */
    Space *space;                /* stage 6's final phase (internally 7): Ramrod in cruiser mode vs the battle cruiser (space.c) */
    Night night; bool night_on;   /* stage 3: night-tinted level-1 tilemap + Hyperjumper */
    Forest forest; bool forest_on; /* stage 4: the rebuilt forest tile layers over the level-1 layer slots */
    bool forest_outro_done;      /* stage 4: the win's radio scene already shown */
    Forest lab; bool lab_on;     /* stage 5: the cave lab (assets/lab, ../lab/compose.py) in the same form; level 1's boss */
    DarkApril dark;              /* stage 5: the second boss, after level 1's (darkapril.c) */
    Power power;                 /* the hero's power attacks on the platform stages (power.c) */
    real hero_speed;            /* the hero's CRHC run speed (April's power raises it for a while) */
    bool walk_in;                /* stages 3/4/5 open with the hero walking in from off screen to walk_stop_x, then walk_script */
    real walk_stop_x; const char *walk_script;
    bool night_taunt_done, night_outro_done;   /* stage 3: the story scenes already shown */
    real state_t;
    real level_t;               /* +0x64 in state 10: music fade-in timer */
    uint32_t dialog_text; real dialog_t;
    Dialog dialog; int dlg_phase; real dlg_focus_x, dlg_focus_y, dlg_t_before, dlg_t_after, dlg_last_cam;   /* state 0xd (FUN_0042d690) */
    real title_t; bool title_on;   /* the level's title card before it starts (game.c title_*) */
    bool debug_collision, free_cam;
    bool dbg_key[8];             /* DBG_KEY_*: debug keys held (free camera) */
} Game;

/* debug keys the platform forwards (keyboards only): F1 collision overlay, F2 free camera, arrows + shift move it */
enum { DBG_KEY_COLLISION, DBG_KEY_FREECAM, DBG_KEY_LEFT, DBG_KEY_RIGHT, DBG_KEY_FAST, DBG_KEY_COUNT };

bool game_init(Game *g, Ren *ren, int sw, int sh, int start_level);   /* start_level: 0 front end, 1 / 2 straight into that level */
void game_debug_key(Game *g, int key, bool down);
void game_update(Game *g, real dt);
void game_draw(Game *g);
