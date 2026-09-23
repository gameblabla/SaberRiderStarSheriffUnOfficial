#pragma once
/* Internal to the Dreamcast platform: the PVR renderer's frame and submission helpers, shared by
 * render_pvr.c (2D API), floor_pvr.c (the Mode-7 plane) and video_dcmv.c (FMV frames). */
#include <kos.h>
#include <dc/pvr.h>
#include <stdint.h>
#include <stdbool.h>
#include "../render.h"

/* the logical -> screen transform (plat_apply_screen) */
typedef struct { float sx, sy, ox, oy; int lw, lh; } PvrView;
extern PvrView pvr_view;

/* frame: rdc_frame_begin opens the translucent list (everything is drawn there, in submission order), rdc_frame_end
 * closes it and hands the scene to the PVR. Draws outside a frame are dropped. */
void rdc_init(void);
void rdc_frame_begin(void);
void rdc_frame_end(void);
bool rdc_in_frame(void);

/* one textured / coloured vertex in screen space (z = 1/w for perspective texturing) */
typedef struct { float x, y, z, u, v; uint32_t argb, oargb; } RdcVert;
/* submit a header (skipped when it is the one sent last) and then a strip / a convex polygon, clipped to the
 * current clip and viewport rectangle */
void rdc_header(const pvr_poly_hdr_t *h);
void rdc_strip(const RdcVert *v, int n);          /* already in strip order, not clipped (caller made sure) */
void rdc_poly(const RdcVert *v, int n);           /* convex polygon in fan order, clipped */
/* the current clip (screen space) */
void rdc_clip_screen(float *x0, float *y0, float *x1, float *y1);

/* compile a header for a texture page (base == NULL: untextured) */
void rdc_compile(pvr_poly_hdr_t *h, pvr_ptr_t base, uint32_t fmt, int tw, int th, RBlend blend, bool linear, bool repeat, bool offset);

/* VRAM: pvr_mem_malloc that retries after asking the game to release textures */
pvr_ptr_t rdc_vram_alloc(size_t bytes);
void rdc_vram_free(pvr_ptr_t p, size_t bytes);
/* forget the header sent last (its memory is going away) */
void rdc_forget_header(void);
