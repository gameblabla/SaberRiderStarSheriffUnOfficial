#pragma once
/* Stage 2 — "The All Galaxy Grand Prix" (anime episode 28): a full-screen SNES-style Mode-7 racer / shooter.
 * Fireball's Red Fury buggy on the New Borderland circuit against Marco Firenza and the Black Hornets, the Hornets'
 * breakaway toward Dome City, the desert pursuit with April on Nova, and the Nerve Center defence against the
 * Hornet leader. Art: assets/mode7.png (tools/build_mode7_assets.py), horizon from the level-1 pack layers. */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdbool.h>
#include "input.h"

typedef struct Mode7 Mode7;

Mode7 *mode7_create(Ren *ren, int sw, int sh, int difficulty, int lives, bool resume_phase2);
void   mode7_destroy(Mode7 *m);
void   mode7_update(Mode7 *m, const Input *in, float dt);
void   mode7_draw(Mode7 *m, bool scanlines);
/* 0 running, 1 stage cleared (after the victory scene), 2 game over */
int    mode7_result(const Mode7 *m);
int    mode7_lives(const Mode7 *m);   /* spare cars left (carried into stage 3) */
bool   mode7_phase2_reached(const Mode7 *m);   /* continue from the pursuit after a phase-2 game over */
