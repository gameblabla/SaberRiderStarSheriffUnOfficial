#pragma once
#include <SDL3/SDL.h>
#include "level.h"
#include "player.h"
#include "input.h"
#include "enemies.h"

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
    int state;              /* 10 playing, 0xd dialog, 0xe level clear, 0xb game over */
    float state_t;
    uint32_t dialog_text; float dialog_t;
    bool debug_collision, free_cam;
    bool key[SDL_SCANCODE_COUNT];
} Game;

bool game_init(Game *g, SDL_Renderer *ren, int sw, int sh);
void game_event(Game *g, const SDL_Event *ev);
void game_update(Game *g, float dt);
void game_draw(Game *g);
