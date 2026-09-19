#pragma once
/* saber_common::Level (LEVL, mode TILE) */
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

#define LVL_MAX_LAYERS 16
#define LVL_MAX_OBJECTS 128

typedef struct {
    uint32_t type;
    float x, y;
    float wp[3][2];          /* waypoints (+0x0c..) */
    float spawn_x, spawn_y;  /* +0x4c,+0x50 */
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
    uint32_t cblock_id;
    const uint32_t *cells;   /* w*h, cell-1 = cblock cell index, 0 = empty */
    CBlock *cb;
} TileMap;

typedef struct {
    char  name[16];
    bool  is_tilemap;
    float parallax;
    int   extra;             /* 1 on SkyBG: wraps horizontally */
    TileMap *map;            /* NULL for sprite layers */
} Layer;

typedef struct {
    uint32_t id;
    char packs[4][12]; int npacks;
    LevelObject objs[LVL_MAX_OBJECTS]; int nobjs;
    Layer layers[LVL_MAX_LAYERS]; int nlayers;
    TileMap maps[LVL_MAX_LAYERS]; int nmaps;
    float width, height;
    int cols, rows, cellw, cellh;
    const uint8_t *collision;   /* cols*rows: bit1 blocks right, bit2 left, bit4 down(floor), bit8 up */
} Level;

bool level_load(Level *L, uint32_t id);
void level_draw_layer(const Level *L, int layer, float cam_x, float cam_y, int screen_w, int screen_h);
uint8_t level_cell(const Level *L, int cx, int cy);
