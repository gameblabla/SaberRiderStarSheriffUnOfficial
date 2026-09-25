#pragma once
/* The heroes' power attacks (ours). The HUD's item counter (next to the lives) holds two a stage; the power button
 * uses one, then a cooldown runs before the next can be used. The world stops under a cut-in:
 *   Saber Rider, Fireball  their anime clips (assets/power, tools/build_power_assets.py) up to the white frame, then
 *                          the game's own flash: every enemy on screen is wiped out and a boss loses a slice of its
 *                          hit points (Fireball's blast hits harder; Saber's slash also clears the enemy shots and
 *                          leaves him untouchable for a moment)
 *   April                  a drawn cut-in (the select screen's portrait, her face from the briefing), then a burst of
 *                          speed with an afterimage trail
 *   Colt                   the same kind of cut-in, then rapid fire
 * In the final phase (space.c) every hero gets the drawn cut-in and it ends in a screen bomb. */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdbool.h>
#include "character.h"
#include "input.h"
#include "video.h"

#define POWER_ITEMS 2
#define POWER_TRAIL 6

enum { PW_IDLE, PW_CUTIN, PW_FLASH };

typedef struct {
    int hero; bool bomb;             /* bomb: the final phase's kind (drawn cut-in, screen bomb for every hero) */
    int items; real cooldown, cooldown_max;
    int phase; real t, dur;         /* the cut-in, then the flash over the running world */
    Video *video;
    bool preload_pending;           /* Saturn hides the next CPK warm-up under the post-movie flash */
    bool strike;                     /* the cut-in has just ended: the caller lands the hit (power_take_strike) */
    real boost_t, boost_max;        /* April's speed / Colt's rapid fire left */
    Character trail[POWER_TRAIL]; real trail_life[POWER_TRAIL]; int trail_i; real trail_t;
} Power;

void power_reset(Power *pw, int hero, bool bomb);      /* a new stage: two items, no cooldown */
bool power_can_start(const Power *pw);
void power_start(Power *pw, Ren *ren);         /* uses an item and opens the cut-in (the caller stops its world) */
bool power_in_cutin(const Power *pw);
void power_update(Power *pw, const Input *in, real dt);   /* every step: the cut-in (skippable), the flash, the timers */
bool power_take_strike(Power *pw);                      /* true once, on the step the cut-in ends */
bool power_speed(const Power *pw);                      /* April's burst is running */
bool power_rapid(const Power *pw);                      /* Colt's */
void power_trail_update(Power *pw, const Character *c, real dt);
void power_draw_trail(const Power *pw, real cam_x, real cam_y);   /* April's afterimages, before the hero */
void power_draw(const Power *pw, Ren *ren, int sw, int sh);   /* the cut-in or the flash, over everything */
void power_draw_hud(const Power *pw, Ren *ren, real x, real y, real w);   /* recharge / effect bar */
void power_close(Power *pw);                            /* frees a clip still open (the stage ends) */
