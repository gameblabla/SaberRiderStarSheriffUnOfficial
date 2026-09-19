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
    bool debug_collision, free_cam;
    bool key[SDL_SCANCODE_COUNT];
} Game;

bool game_init(Game *g, SDL_Renderer *ren, int sw, int sh);
void game_event(Game *g, const SDL_Event *ev);
void game_update(Game *g, float dt);
void game_draw(Game *g);
