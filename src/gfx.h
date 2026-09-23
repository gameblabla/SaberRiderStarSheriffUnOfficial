#pragma once
/* Graphics resources decoded from the packs: cblocks (tile banks + cell grids) and sprites. */
#include "platform/render.h"
#include "platform/plat.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t id;
    int frames, cols, rows;     /* cell grid per frame */
    int tw, th, ntiles;
    const uint16_t *cells;      /* frames*cols*rows, 0xFFFF = empty (our copy) */
    const uint8_t  *mask;
    RTex *tex;           /* tile sheet, TILES_PER_ROW tiles wide; NULL while evicted (cblock_tex brings it back) */
    int sheet_cols;
    bool from_pack;      /* can be rebuilt from its pack block (so its texture may be evicted) */
    const char *file;    /* or from this PNG (cblock_from_png), likewise evictable */
    uint32_t last_used;  /* gfx frame of the last draw */
} CBlock;

typedef struct Sprite {
    uint32_t id;
    int w, h, frames;
    RTex *tex;           /* frames laid out horizontally, each POT-padded frame cropped to w×h; NULL while evicted */
    bool from_pack;
    const char *file;    /* the PNG it can be rebuilt from (sprite_from_png), so it may be evicted like a pack one */
    uint32_t last_used;
} Sprite;

bool  gfx_init(Ren *r);
/* once per drawn frame: the clock that decides which textures are idle enough to evict under memory pressure */
void  gfx_frame(void);
/* the texture to draw with (reloaded from the pack if it had been evicted) */
RTex *sprite_tex(const Sprite *s);
RTex *cblock_tex(const CBlock *c);
CBlock *cblock_get(uint32_t id);          /* cached */
/* a cblock made of our own RGBA sheet: one frame of (w/tw) x (h/th) cells, cell i = tile i (recreated heroes) */
CBlock *cblock_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int tw, int th);
/* the same from a PNG file, kept by path so its texture can be dropped under memory pressure and decoded again */
CBlock *cblock_from_png(uint32_t id, const char *path, int tw, int th);
Sprite *sprite_get(uint32_t id);          /* cached */
Sprite *sprite_from_blob(uint32_t id, const uint8_t *blob, uint32_t size);   /* decode a sprite blob not in a pack (fonts) */
Sprite *sprite_from_rgba(uint32_t id, const uint32_t *px, int w, int h, int frames);   /* register our own RGBA image (frames side by side) under a resource id */
Sprite *sprite_from_png(uint32_t id, const char *path, int frame_w);  /* the same from a PNG, evictable (frame_w 0 = one frame) */
int   cblock_ncells(const CBlock *c);
/* night-stage recolor: tint a tile bank (SDL texture color mod; 255,255,255
 * resets). The tint is texture state, so set it around a layer's draw. */
void  cblock_tint(const CBlock *c, uint8_t r, uint8_t g, uint8_t b);
/* draw tile t at x,y (screen space, integer) */
void  cblock_draw_tile(const CBlock *c, int t, float x, float y, bool flip);
/* draw a full frame (cols×rows cells) with its top-left at x,y */
void  cblock_draw_frame(const CBlock *c, int frame, float x, float y, bool flip);
void  sprite_draw(const Sprite *s, int frame, float x, float y, bool flip);
void  sprite_draw_scaled(const Sprite *s, int frame, float x, float y, float w, float h);
void  sprite_draw_scaled_mod(const Sprite *s, int frame, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
/* draw centred at cx,cy, rotated by angle (degrees), with a brightness/alpha modulation (255 = unchanged) */
void  sprite_draw_rotated(const Sprite *s, int frame, float cx, float cy, float scale, float angle, uint8_t bright, uint8_t alpha);
void  sprite_draw_mod(const Sprite *s, int frame, float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
