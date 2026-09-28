#pragma once
#include "platform/render.h"
#include "platform/plat.h"
/* In-game HUD (port of the HUD part of FUN_0042e2f0). max_hearts: the heart slots (0..4, by difficulty), hearts: those full. */
void hud_draw(Ren *r, int character, int max_hearts, int lives, int hearts, int ammo);
/* a HUD backdrop's alpha: see-through, but opaque on the Saturn (VDP1's half-transparency is meshed over the VDP2
 * planes, a dither of dots behind the counters and bars) */
#ifdef PLAT_SATURN
#define HUD_ALPHA(a) 255
#else
#define HUD_ALPHA(a) (a)
#endif
