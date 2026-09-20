#pragma once
/* Recreated heroes. The demo ships every hero's CRHC (Fireball 9C8F9A9E, April 79260A58, Colt 26818B85) but only
 * Fireball's cblock; April's sheet is our reconstruction (../heroes/, packed by build_april_engine_sheet.py into
 * assets/april.png on Fireball's cell layout plus extra rows for the families she has more frames for). */
#include <stdbool.h>
#include "character.h"

enum { HERO_SABER = 0, HERO_FIREBALL = 1, HERO_APRIL = 2, HERO_COLT = 3 };

bool hero_available(int character);          /* selectable on the character select screen */
/* after character_init parsed the CRHC: swap in the recreated sheet + animation table changes; false = keep Fireball's */
bool hero_apply(Character *c);
const char *hero_name(int character);
