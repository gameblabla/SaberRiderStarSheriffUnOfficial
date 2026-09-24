#pragma once
/* Platform render interface: an immediate-mode 2D API (the subset of SDL's renderer the game uses, same
 * semantics) plus one high-level primitive, the perspective floor plane of the Mode-7 stages.
 *
 * Core game code only talks to this header. Each platform implements it:
 *   platform/sdl3/render_sdl.c       SDL3 renderer (PC)
 *   platform/dreamcast/render_pvr.c  PowerVR through KOS direct rendering (Dreamcast)
 *   platform/null/render_null.c      no output (headless tests, bring-up of a new port)
 * Coordinates are logical screen pixels (plat_set_logical) as `real` (real.h: float on the PC and the Dreamcast, 16.16
 * fixed point on the Saturn); a backend scales them to its display.
 * Textures carry their own colour/alpha modulation, blend and scale mode like SDL textures, so a draw
 * reads that state at the time of the call.
 *
 * Porting notes for fixed-function / tile hardware (Saturn VDP1/2, SNES, PC Engine, PC-FX): every draw
 * the game makes is a (possibly flipped, scaled, rotated, colour-modulated) rectangle out of a texture, a
 * solid rectangle or line, or r_floor_draw. A backend without arbitrary textures maps textures created
 * from pack sprites / tile banks to its own sprite or character memory (gfx.c creates every one of them
 * through rtex_create, with the resource id available via rtex_set_tag) and draws the floor with its
 * hardware rotation plane (VDP2 RBG0, SNES mode 7), which is why the floor is not expressed as polygons. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../real.h"

typedef struct Ren Ren;       /* the renderer (one per program; passed along like SDL_Renderer) */
typedef struct RTex RTex;     /* a texture */

typedef struct { real x, y, w, h; } RFRect;
typedef struct { int x, y, w, h; } RRect;
typedef struct { real x, y; } RFPoint;
typedef struct { real r, g, b, a; } RFColor;
typedef struct { RFPoint position; RFColor color; RFPoint tex_coord; } RVertex;

typedef enum { R_BLEND_NONE, R_BLEND_BLEND, R_BLEND_ADD } RBlend;
typedef enum { R_FLIP_NONE = 0, R_FLIP_H = 1, R_FLIP_V = 2 } RFlip;
typedef enum { R_SCALE_NEAREST, R_SCALE_LINEAR } RScale;
typedef enum { R_TEX_STATIC, R_TEX_STREAMING } RTexAccess;

/* ---- textures: pixels are RGBA8888 in byte order (R first; SDL_PIXELFORMAT_ABGR8888 on little endian) ---- */
RTex *rtex_create(Ren *r, int w, int h, RTexAccess access, const uint32_t *px);   /* px may be NULL (cleared) */
void  rtex_update(RTex *t, const uint32_t *px, int pitch_bytes);                   /* whole texture */
/* a static texture whose pixels come from rows(ud, y0, n, out): rows y0..y0+n-1, n x w pixels into out. Lets a
 * backend convert a big image piece by piece instead of holding it whole (the callback may run twice per row). */
typedef void (*RTexRows)(void *ud, int y0, int n, uint32_t *out);
RTex *rtex_create_rows(Ren *r, int w, int h, RTexRows rows, void *ud);
/* a texture baked ahead of time in the backend's own format (a "PVT1" block of the console's tex.pck, see
 * tools/dc/texbake.py); NULL where the backend has none. The block must be 32-byte aligned and may be altered (the
 * palette indices are remapped to where its colours land); it can be freed afterwards. */
RTex *rtex_create_baked(Ren *r, uint8_t *block, size_t size);
void  rtex_destroy(RTex *t);
void  rtex_size(const RTex *t, int *w, int *h);
void  rtex_set_color_mod(RTex *t, uint8_t r, uint8_t g, uint8_t b);
void  rtex_set_alpha_mod(RTex *t, uint8_t a);
void  rtex_set_blend(RTex *t, RBlend b);                                            /* default R_BLEND_BLEND */
void  rtex_set_scale(RTex *t, RScale s);                                            /* default R_SCALE_NEAREST */
void  rtex_set_tag(RTex *t, uint32_t tag);                                          /* resource id (diagnostics, sprite-hardware ports) */
Ren  *rtex_renderer(const RTex *t);
/* memory pressure: a backend with a small texture memory calls hook() when an allocation fails and retries while it
 * returns true (it released something); the game's texture caches register it (gfx.c) */
void  r_set_evict_hook(bool (*hook)(void));

/* ---- draw state ---- */
void r_set_draw_color(Ren *r, uint8_t R, uint8_t G, uint8_t B, uint8_t A);
void r_set_draw_blend(Ren *r, RBlend b);
void r_set_clip(Ren *r, const RRect *clip);          /* NULL = none; in viewport coordinates */
void r_set_viewport(Ren *r, const RRect *vp);        /* NULL = whole screen; draws are offset by vp->x,y and clipped to it */
bool r_rect_intersect(const RRect *a, const RRect *b, RRect *out);

/* ---- primitives ---- */
void r_clear(Ren *r);                                /* whole screen, current draw colour */
void r_fill_rect(Ren *r, const RFRect *q);           /* NULL = whole viewport */
void r_fill_rects(Ren *r, const RFRect *q, int n);
void r_rect(Ren *r, const RFRect *q);                /* 1 px outline */
void r_line(Ren *r, real x0, real y0, real x1, real y1);
void r_point(Ren *r, real x, real y);
void r_tex(Ren *r, RTex *t, const RFRect *src, const RFRect *dst);   /* NULL src = whole texture, NULL dst = whole viewport */
/* rotated clockwise by angle degrees around center (relative to dst; NULL = its centre), then flipped */
void r_tex_rot(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, rdeg angle, const RFPoint *center, RFlip flip);
/* n unrotated copies out of one texture in one call (a tile layer, a character's cells): src[i] -> dst[i], a negative
 * dst[i].w mirrors that one horizontally (its left edge stays dst[i].x). The same as n r_tex calls, without the
 * per-call overhead: a thousand 16 px tiles a frame is most of what the game draws. */
void r_tex_batch(Ren *r, RTex *t, const RFRect *src, const RFRect *dst, int n);
/* triangles; t may be NULL (colour only, draw blend mode) */
void r_geometry(Ren *r, RTex *t, const RVertex *v, int nv, const int *idx, int ni);

/* ---- the perspective floor plane (Mode 7) ----
 * A material map of mapn x mapn cells (power of two, wraps) of (1 << cell_shift) world units; each cell holds a
 * material index. Material m is a tex x tex texture with `mips` box-filtered levels (level L is (tex >> L)
 * square, RGBA like textures), sampled by world position: 1 texel = 1 world unit at level 0, repeating every
 * tex units, so a material continues seamlessly across cells. The cells array stays owned by the caller; call
 * r_floor_cells_changed after editing it. */
typedef struct RFloor RFloor;
typedef struct {
    int mapn, cell_shift; const uint8_t *cells;
    int tex, mips, nmat;
    const uint32_t *const *mat;      /* mat[m * mips + L] */
} RFloorDesc;
typedef struct {
    real cam_x, cam_y, fx, fy;       /* eye position on the ground plane, unit forward vector */
    real cam_h, focal, horizon;      /* eye height (world units), focal length (px), horizon (screen row, may be fractional) */
    int y0, y1;                      /* screen rows drawn: [y0, y1) */
    real row_off;                    /* where a row is sampled: 0 its top edge, 0.5 its centre */
    real fog0, fog1; int fog_max;   /* linear distance fog to `haze`, fog_max/256 at fog1 and beyond */
    uint32_t haze;                   /* RGBA like texture pixels (0xAABBGGRR as an integer) */
    real mip_step;                  /* world units per pixel at which mip 1 starts; mip L from mip_step * 2^(L-1) */
    int sw;                          /* width drawn (from x 0) */
} RFloorView;
RFloor *r_floor_create(Ren *r, const RFloorDesc *d);
void    r_floor_cells_changed(RFloor *f);
void    r_floor_draw(Ren *r, RFloor *f, const RFloorView *v);
void    r_floor_destroy(RFloor *f);

/* ---- retained tile layers ----
 * A backend with hardware scroll planes (the Saturn's VDP2) may hold a level's tile layers itself, baked ahead of time
 * for that level. level_draw_layer asks it first: true means the backend shows layer `layer` of level `level` (its
 * id) for this camera position this frame, and the core draws nothing for it; false (every other backend) means the
 * core draws the tiles. */
bool r_layer(Ren *r, uint32_t level, int layer, real cam_x, real cam_y);
/* The level layer the sprites that follow belong to (-1: none, in front of everything: HUD, menus). A backend whose
 * layers aren't drawn in the core's order (hardware planes composited by priority) places them by it; the others ignore
 * it (painter's order already holds). */
void r_set_depth(Ren *r, int layer);
