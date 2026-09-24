#pragma once
/* saber_common::Level (LEVL, mode TILE) */
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

#define LVL_MAX_LAYERS 16
#define LVL_MAX_OBJECTS 128

typedef struct {
    uint32_t type;
    real x, y;               /* fixed point: saturated at +-32768 (the data's +-100000 "off screen" waypoints) */
    real wp[3][2];          /* waypoints (+0x0c..) */
    real spawn_x, spawn_y;  /* +0x4c,+0x50 */
    uint32_t layer;          /* +0x54 */
    uint16_t a;              /* +0x58 */
    uint8_t  n_wp;           /* +0x5a */
    int8_t   loops;          /* +0x5b */
    uint16_t b;              /* +0x5c */
    uint8_t  c, d;           /* +0x5e,+0x5f */
    const uint8_t *raw;      /* full 128-byte record */
} LevelObject;

typedef struct {
    int w, h;
    int used_w;              /* wrap layers repeat every used_w columns (FUN_0040dce0 trims to the last non-empty column) */
    uint32_t cblock_id;
    const uint32_t *cells;   /* w*h, cell-1 = cblock cell index, 0 = empty */
    CBlock *cb;
} TileMap;

typedef struct {
    char  name[16];
    bool  is_tilemap;
    real parallax;
    int   extra;             /* 1 on SkyBG: wraps horizontally */
    TileMap *map;            /* NULL for sprite layers */
} Layer;

typedef struct {
    uint32_t id;
    uint32_t planes_id;         /* the tile layers' set for a backend that holds them (r_layer): the level id, or a stage's own */
    char packs[4][12]; int npacks;
    LevelObject objs[LVL_MAX_OBJECTS]; int nobjs;
    Layer layers[LVL_MAX_LAYERS]; int nlayers;
    TileMap maps[LVL_MAX_LAYERS]; int nmaps;
    real width, height;
    int cols, rows, cellw, cellh;
    const uint8_t *collision;   /* cols*rows: bit1 blocks right, bit2 left, bit4 down(floor), bit8 up */
} Level;

/* the level data's +-100000 waypoints: just outside the screen's right / left edge (enemies.c). Fixed point saturates
 * them (r_bits) to +-32768, beyond any level. */
#ifdef REAL_FIXED
#define WP_OFF_R   FX_MAX
#define WP_OFF_L   (-FX_MAX)
#define WP_OFF_TEST FX(32000)   /* beyond it: off screen */
#else
#define WP_OFF_R   100000.0f
#define WP_OFF_L   -100000.0f
#define WP_OFF_TEST 99999.0f
#endif

bool level_load(Level *L, uint32_t id);
void level_draw_layer(const Level *L, int layer, real cam_x, real cam_y, int screen_w, int screen_h);
/* debug (SABER_DUMPLAYERS, tools/saturn/layers.py): the tile layers as the stage built them, as text */
void level_dump_layers(const Level *L, const char *path);
uint8_t level_cell(const Level *L, int cx, int cy);
