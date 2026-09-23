#pragma once
/* Stage 3 — Hyperjumper Pass.
 *
 * A night run through a new desert route built from whole level-1 scenes
 * (night_level.c), every level-1 layer kept for the parallax and night-tinted,
 * under a dark sky with the red moon behind everything. At the end of the
 * route the camera stops on open ground and Hyperjumper arrives the way the
 * level-1 boss does: boss music and the engine boot, a pass far behind the
 * mesas, a nearer pass, then it comes in to fight. It hovers and fires its
 * pilot gun diagonally, skims the ground (jump over it or slide under it),
 * and hangs overhead in its front pose raining straight-down bolts. */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "level.h"
#include "night_level.h"
#include "player.h"
#include "bullets.h"

#define HYPERJUMPER_SHOTS 48

typedef struct {
    bool alive, front, flip;
    float x, y, vx, vy;
} HyperjumperShot;

typedef enum {
    HJ_DORMANT, HJ_FAR_PASS, HJ_FAR_GAP, HJ_MID_PASS, HJ_MID_GAP,
    HJ_SIDE_IN, HJ_SIDE_HOLD, HJ_SIDE_OUT, HJ_LOW_WARN, HJ_LOW_PASS,
    HJ_FRONT_IN, HJ_FRONT_FIRE, HJ_FRONT_OUT, HJ_DYING, HJ_DONE,
} HyperState;

typedef struct {
    Stage3World world;
    int difficulty;
    float start_x, start_y;
    int far_layer, mid_layer, play_layer;   /* level-1 sprite layers "Small Hyperjmpr", "MidBGHyperjpr", "PlayerSprites" */

    Sprite *sky, *moon;
    Sprite *side_normal, *side_boost, *side_fire1, *side_fire2;
    Sprite *front_idle, *front_fire, *projectile_diagonal, *projectile_front;

    HyperState state;
    float st;                   /* time in the current state */
    float arena_x;              /* camera x of the arena (fixed once the fight starts) */
    int sw;
    float bx, by, speed;        /* ship centre (world), current speed */
    int dir;                    /* -1 flying/facing left (art as drawn), +1 right (mirrored) */
    int cycle;                  /* attack round: sides and passes alternate */
    int hp, hp_max;
    float hit_flash;
    bool shooting; float fire_t;   /* gun cycle: running this step, time to the next shot */
    float death_vy; int death_phase; bool death_front;
    bool boss_started, clear_ready;
    HyperjumperShot shots[HYPERJUMPER_SHOTS];
} Night;

bool night_init(Night *n, Level *L, int difficulty);
void night_dispose(Night *n);
void night_update(Night *n, Player *pl, Bullets *pb, Effects *fx, const Level *L,
                  float cam_x, int sw, float dt, bool live);

/* per-layer night palette for the level-1 tile banks (false: skip the layer) */
bool night_layer_tint(const Night *n, const Layer *ly, uint8_t *r, uint8_t *g, uint8_t *b);
void night_draw_background(Night *n, float cam_x, int sw, int sh);
void night_draw_layer(Night *n, SDL_Renderer *ren, int layer, float cam_x, float cam_y);   /* boss + shots in their sprite layer */
void night_draw_hud(Night *n, SDL_Renderer *ren, int sw, int sh);
