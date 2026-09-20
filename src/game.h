#pragma once
#include <SDL3/SDL.h>
#include "level.h"
#include "player.h"
#include "input.h"
#include "enemies.h"
#include "dialog.h"
#include "menu.h"

typedef struct {
    SDL_Renderer *ren;
    int sw, sh;
    Level level;
    PhysicsWorld world;
    Player player;
    Bullets player_bullets, enemy_bullets;
    Effects effects;
    Enemies enemies;
    int player_layer;
    Input in;
    float cam_x, cam_y;
    bool cam_locked;
    /* level flow zones (from LEVL objects) */
    struct { float cx, cy, hx, hy; bool armed; } stops[4]; int nstops;        /* 998/999 */
    struct { float cx, cy, hx, hy; bool done; uint32_t text; float focus_x, focus_y, t_in, t_out; } dialogs[4];
    struct { float cx, cy, hx, hy; bool set; } exit_zone;                     /* type 4 */
    struct { float cx, cy, hx, hy, rx, ry; } deathzones[8]; int ndeath;       /* type 3 */
    int state;              /* 10 playing, 0xd dialog, 0xe level clear, 0xb game over, 0xc pause */
    Menu menu; bool in_level;
    float state_t;
    float level_t;               /* +0x64 in state 10: music fade-in timer */
    uint32_t dialog_text; float dialog_t;
    Dialog dialog; int dlg_phase; float dlg_focus_x, dlg_focus_y, dlg_t_before, dlg_t_after, dlg_last_cam;   /* state 0xd (FUN_0042d690) */
    bool debug_collision, free_cam;
    bool key[SDL_SCANCODE_COUNT];
} Game;

bool game_init(Game *g, SDL_Renderer *ren, int sw, int sh);
void game_event(Game *g, const SDL_Event *ev);
void game_update(Game *g, float dt);
void game_draw(Game *g);
