#pragma once
/* Graphics resources decoded from the packs: cblocks (tile banks + cell grids) and sprites. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t id;
    int frames, cols, rows;     /* cell grid per frame */
    int tw, th, ntiles;
    const uint16_t *cells;      /* frames*cols*rows, 0xFFFF = empty (points into pack memory) */
    const uint8_t  *mask;
    SDL_Texture *tex;           /* tile sheet, TILES_PER_ROW tiles wide */
    int sheet_cols;
} CBlock;

typedef struct {
    uint32_t id;
    int w, h, frames;
    SDL_Texture *tex;           /* frames laid out horizontally, each POT-padded frame cropped to w×h */
} Sprite;

bool  gfx_init(SDL_Renderer *r);
CBlock *cblock_get(uint32_t id);          /* cached */
Sprite *sprite_get(uint32_t id);          /* cached */
int   cblock_ncells(const CBlock *c);
/* draw tile t at x,y (screen space, integer) */
void  cblock_draw_tile(const CBlock *c, int t, float x, float y, bool flip);
/* draw a full frame (cols×rows cells) with its top-left at x,y */
void  cblock_draw_frame(const CBlock *c, int frame, float x, float y, bool flip);
void  sprite_draw(const Sprite *s, int frame, float x, float y, bool flip);
void  sprite_draw_scaled(const Sprite *s, int frame, float x, float y, float w, float h);
