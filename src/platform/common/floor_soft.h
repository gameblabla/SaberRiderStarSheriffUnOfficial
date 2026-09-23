#pragma once
/* The floor plane rasterised per scanline on the CPU into a streaming texture (render.h's r_floor_* for backends
 * with streaming textures and a fast CPU). Pixel-identical to the original software Mode-7 renderer. */
#include "../render.h"
RFloor *floor_soft_create(Ren *r, const RFloorDesc *d);
void    floor_soft_draw(Ren *r, RFloor *f, const RFloorView *v);
void    floor_soft_destroy(RFloor *f);
