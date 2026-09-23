#pragma once
/* Stage 6, final phase — "The Outrider Battle Cruiser": right after the Renegades fall, Ramrod goes back to cruiser
 * mode and chases the ship that dropped them. A horizontal shooter in space (the team's own mock-up, "RAMROD
 * sidescrolling space battle", and the Dny9A6 clip of the brown cruiser firing its nose laser): the minefield it left
 * behind, its fighter squadrons, then the battle cruiser itself - gun ports, the nose laser, launched swarms and mines -
 * until it blows apart. Art: assets/space (../space/build.py). */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include "input.h"

typedef struct Space Space;

Space *space_create(SDL_Renderer *ren, int sw, int sh, int difficulty, int lives);
void   space_destroy(Space *s);
void   space_update(Space *s, const Input *in, float dt);
void   space_draw(Space *s, bool scanlines);
/* 0 running, 1 the cruiser destroyed (after the outro scene), 2 game over */
int    space_result(const Space *s);
int    space_lives(const Space *s);
