#pragma once
/* Stage 6, phase 1 — "Ramrod, Power Stride": first person from Ramrod's cockpit, robot mode, on the Yuma desert
 * outside the frontier outpost (series pilot "Star Sheriff Round-Up": the Outriders march their Renegade battle
 * mechs on the town). A Mode-7 floor under a 360-degree panorama, the Outrider mechs as scaled sprites; Ramrod
 * shoots (hold, the guns heat up) or punches (close range, alternating fists, the DC881 clip's arm).
 * Three waves, radio scenes between them. Art: assets/ramrod (../ramrod pipeline). */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdbool.h>
#include "input.h"

typedef struct Ramrod Ramrod;

Ramrod *ramrod_create(Ren *ren, int sw, int sh, int difficulty, int lives);
void    ramrod_destroy(Ramrod *r);
void    ramrod_update(Ramrod *r, const Input *in, real dt);
void    ramrod_draw(Ramrod *r, bool scanlines);
/* 0 running, 1 phase cleared (after the outro scene; the final phase, space.c, follows), 2 game over */
int     ramrod_result(const Ramrod *r);
int     ramrod_lives(const Ramrod *r);
