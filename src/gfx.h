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
    bool retained;       /* active scene dependency: eviction would cause an unplanned data read */
    const char *file;    /* or from this PNG (cblock_from_png), likewise evictable */
    uint32_t last_used;  /* gfx frame of the last draw */
    RTex *ftex;          /* the Saturn: its frames baked whole (tools/saturn/build_disc.py FRAME_BAKED); NULL while evicted */
    int fstate, fper_row;   /* ftex: 0 not looked for yet, 1 baked (fper_row frames a row), -1 none */
    bool frames_only;    /* the scene draws it by whole frames only (gfx_keep_cblock_frames): ftex kept, no tile sheet */
} CBlock;

typedef struct Sprite {
    uint32_t id;
    int w, h, frames;
    RTex *tex;           /* frames laid out horizontally, each POT-padded frame cropped to w×h; NULL while evicted */
    bool from_pack;
    bool retained;
    const char *file;    /* the PNG it can be rebuilt from (sprite_from_png), so it may be evicted like a pack one */
    uint32_t last_used;
} Sprite;

bool  gfx_init(Ren *r);
Ren  *gfx_renderer(void);
/* the SCANLINES option: every other row of the sw x sh screen darkened, in one draw call */
void  gfx_scanlines(int sw, int sh);
/* forget every cached sprite and cblock (tools/dc/texprep walks all of the packs' graphics through the cache) */
void  gfx_flush(void);
void  gfx_trim(void);   /* drop every pack / PNG texture (made again on the next draw): a stage change */
bool  gfx_keep_sprite(const Sprite *s);   /* keep a prepared scene texture out of both eviction paths */
bool  gfx_keep_cblock(const CBlock *c);
/* the Saturn: a cblock the scene only draws by whole frames (cblock_draw_frame) - its frames baked whole are kept and
 * its tile sheet let go (the power cut-in's pieces: 66 KB less for the stage); elsewhere gfx_keep_cblock */
bool  gfx_keep_cblock_frames(const CBlock *c);
void  gfx_keep_loaded(void);              /* retain the warmed scene set at the music ownership boundary */
/* the same, but reload anything the load tail already evicted before keeping it (the disc is still unlocked here):
 * returns how many textures could not be made resident, each named on stderr. Without this, a texture dropped during
 * the load is flagged as present and its first draw becomes a locked read - a missing texture during gameplay. */
unsigned gfx_prepare_scene(void);
/* Saturn: from the music ownership boundary (game.c, just before music_play) to the next gfx_trim(), a draw must not
 * read the disc. The drive serves one thing at a time, so such a read stops the CD-DA music dead for the length of
 * the seek (see cd_sat.c read_sectors -> stream_start -> cdda_interrupt). While locked, a texture that was evicted
 * under memory pressure comes back as NULL - a missing texture and a logged id - never as a seek. gfx_trim() unlocks. */
void  gfx_lock_reads(void);
void  gfx_unlock_reads(void);
unsigned gfx_locked_reads(void);         /* textures a locked draw could not reload: 0 = the scene stayed fully resident */
/* the texture of one of our images (assets/): the console's baked one, else the PNG decoded; w, h may be NULL */
RTex *gfx_image_tex(const char *path, int *w, int *h);
/* once per drawn frame: the clock that decides which textures are idle enough to evict under memory pressure */
void  gfx_frame(void);
/* the texture to draw with (reloaded from the pack if it had been evicted) */
RTex *sprite_tex(const Sprite *s);
RTex *cblock_tex(const CBlock *c);
void  cblock_preload(const CBlock *c);   /* its textures now (cblock_tex, and the Saturn's baked frames) */
CBlock *cblock_get(uint32_t id);          /* cached */
void  cblock_unload(const CBlock *c);     /* drop its texture now (it is loaded again if the cblock is drawn) */
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
void  cblock_draw_tile(const CBlock *c, int t, real x, real y, bool flip);
/* Draw from first_row to the bottom, retaining the cell's original anchor. */
void  cblock_draw_tile_rows(const CBlock *c, int t, real x, real y, bool flip, int first_row);
/* many tiles of one bank in one draw call: begin, a tile at a time (screen space, integer), end. Nothing else may be
 * drawn in between. */
void  cblock_batch_begin(const CBlock *c);
void  cblock_batch_tile(int t, real x, real y, bool flip);
void  cblock_batch_end(void);
/* draw a full frame (cols×rows cells) with its top-left at x,y */
void  cblock_draw_frame(const CBlock *c, int frame, real x, real y, bool flip);
void  sprite_draw(const Sprite *s, int frame, real x, real y, bool flip);
void  sprite_draw_scaled(const Sprite *s, int frame, real x, real y, real w, real h);
void  sprite_draw_scaled_mod(const Sprite *s, int frame, real x, real y, real w, real h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
/* draw centred at cx,cy, rotated by angle (degrees), with a brightness/alpha modulation (255 = unchanged) */
void  sprite_draw_rotated(const Sprite *s, int frame, real cx, real cy, real scale, real angle, uint8_t bright, uint8_t alpha);
void  sprite_draw_mod(const Sprite *s, int frame, real x, real y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);
