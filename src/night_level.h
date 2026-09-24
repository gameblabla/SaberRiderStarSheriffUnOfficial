#pragma once
/* Stage 3 world: a new desert route built from whole scenes of the level-1
 * tilemaps.
 *
 * Every layer that scrolls with the play plane (Playfield, Platforms, Cars,
 * ForegroundStuff) and the collision grid are cut at the same 16 px columns,
 * only where no rock, car, pad or house crosses the cut, and re-laid in a new
 * order (some scenes mirrored). The background layers are rebuilt the same way
 * in their own parallax space, from their rock-only stretches, so the town's
 * houses never show. Cells with bit 31 set are drawn mirrored. */
#include <stdbool.h>
#include <stdint.h>
#include "level.h"

#define STAGE3_CELL_FLIP 0x80000000u

typedef struct {
    uint32_t *cells[LVL_MAX_LAYERS];   /* owned replacement cell arrays (NULL: layer unchanged) */
    uint8_t *collision;                /* owned replacement collision grid */
    real width;
} Stage3World;

/* Rebuilds L in place (maps, collision, width); the pack data it pointed at is
 * untouched. On failure nothing in L is changed. */
bool stage3_world_build(Level *L, Stage3World *w);
void stage3_world_free(Stage3World *w);

/* Enemy triggers of the route, in level-1 LEVL object form (fed to
 * enemies_add_trigger). */
int stage3_triggers(LevelObject *out, int max, int player_layer);
