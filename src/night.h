#pragma once
/* Stage 3 — Hyperjumper Pass.
 *
 * The level-1 tile banks are reused with a night tint; the old level's object
 * triggers are not installed, its collision grid and tilemap cell order are
 * rearranged for the pass/platform layout, and the boss uses the supplied
 * Hyperjumper side/front artwork. */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "level.h"
#include "player.h"
#include "bullets.h"

#define HYPERJUMPER_SHOTS 64
#define STAGE3_STATIC_ART 6

typedef struct {
    bool alive;
    float x, y, vx, vy, ttl, angle;
    bool front;
} HyperjumperShot;

enum { HYPER_READY, HYPER_SWEEP, HYPER_FRONT, HYPER_DEATH };

typedef struct {
    bool active;
    float t;
    int difficulty;

    /* Stage 3 owns fresh collision and tilemap cell arrays. The source LEVL
     * tile banks are reused, but its long run is rearranged into a new pass. */
    uint8_t *arena_collision;
    uint32_t *map_cells[LVL_MAX_LAYERS];
    float arena_width;
    float start_x, start_y;

    /* Generated stage-3 sky/moon, then boss art loaded from assets/hyperjumper/. */
    Sprite *sky, *moon;
    Sprite *static_art[STAGE3_STATIC_ART];
    Sprite *side_normal, *side_boost, *side_fire1, *side_fire2;
    Sprite *front_idle, *front_fire, *projectile_diagonal, *projectile_front;

    int boss_state;
    bool boss_alive, clear_ready;
    int hp, hp_max, passes;
    float boss_x, boss_y, state_t, boost_t, shot_t, body_hit_cd;
    int boss_dir;
    float hit_flash, death_t;
    HyperjumperShot shots[HYPERJUMPER_SHOTS];

    char msg[48];
    float msg_t, white;
} Night;

void night_init(Night *n, Level *L, int difficulty);
void night_dispose(Night *n);
void night_update(Night *n, Player *pl, Bullets *pb, Effects *fx,
                  float cam_x, int sw, int sh, float dt, bool live);

void night_draw_boss(Night *n, SDL_Renderer *ren, float cam_x, float cam_y);
void night_draw_background(Night *n, float cam_x, float cam_y, int sw, int sh);
void night_draw_static(Night *n, float cam_x, float cam_y, int sw);
void night_draw_projectiles(Night *n, SDL_Renderer *ren, float cam_x, float cam_y);
void night_draw_banner(Night *n, SDL_Renderer *ren, int sw, int sh);
