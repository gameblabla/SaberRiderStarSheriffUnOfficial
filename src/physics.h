#pragma once
/* Port of saber_game::Physics (src/game/physics.cpp). AABB vs 8x8 collision cells.
 * Cell bits: 1 blocks moving right, 2 blocks moving left, 4 blocks moving down (floor), 8 blocks moving up. */
#include <stdint.h>
#include <stdbool.h>
#include "level.h"
#include "real.h"

enum { PHYS_IGNORE_DOWN = 1, PHYS_IGNORE_LEFT = 2, PHYS_IGNORE_UP = 4, PHYS_IGNORE_RIGHT = 8, PHYS_NO_GRAVITY = 0x10 };
enum { COLL_DOWN = 1, COLL_LEFT = 2, COLL_UP = 4, COLL_RIGHT = 8 };
/* cell bit 0x10: ramp ground (with 4, filled from the surface down): bodies walk up and down its 8 px steps */
enum { COLL_RAMP = 0x10 };

typedef struct {
    real x, y;          /* transform position (sprite center) */
    real ox, oy;        /* collider center offset from transform */
    real hx, hy;        /* half extents */
    real vx, vy;
    uint8_t flags;       /* PHYS_* */
    uint8_t coll;        /* COLL_* result of last step */
    uint8_t ground_tile; /* cell value under the feet when COLL_DOWN */
} Body;

typedef struct {
    real gx, gy;
    real world_min_x, world_max_x;
} PhysicsWorld;

void physics_step(const PhysicsWorld *w, const Level *L, Body *b, real dt);
