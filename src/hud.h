#pragma once
#include "platform/render.h"
#include "platform/plat.h"
/* In-game HUD (port of the HUD part of FUN_0042e2f0). difficulty: 0 easy (4 hearts), 1 normal (2), 2 hard (none). */
void hud_draw(Ren *r, int character, int difficulty, int lives, int hearts, int ammo);
