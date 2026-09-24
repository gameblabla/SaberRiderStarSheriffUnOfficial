#pragma once
/* Shared between the Saturn platform files (src/platform/saturn). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* the screen: 320x224 (4:3, NORMAL_A) or 352x224 (wide, NORMAL_B), 224 lines, non-interlaced (plan 3) */
#define SAT_SCREEN_H 224
#define SAT_WIDE_W   352

/* cd_sat.c: the ISO's file list (fopen reads the CD through it) */
void cd_sat_init(void);
void cd_sat_stats(unsigned long *reads, unsigned long *bytes, unsigned long *seeks);
void cd_sat_stream_stop(void);   /* the data stream gives the drive up (CD-DA takes it) */

/* log_sat.c: stdout / stderr -> the RAM log ring the harness reads */
void log_sat_write(const char *s, size_t n);

/* plat_sat.c: timing from the master SH-2's free-running timer (FRT, clock / 32) */
void     sat_timer_init(void);
uint32_t sat_timer_us(void);          /* microseconds since boot (wraps after ~71 minutes) */
uint32_t sat_vblanks(void);           /* vertical blanks since boot */
void     sat_vblank_tick(void);       /* from the vblank-out handler */

/* render_sat.c: VDP1 */
typedef struct Ren Ren;
Ren *rsat_renderer(void);
void rsat_init(void);
void rsat_set_screen(int w, int h);
void rsat_frame_begin(void);
void rsat_frame_end(void);
int  rsat_prims(void);
void rsat_stats(unsigned *parts_resident, unsigned *vram_used, unsigned *uploads, unsigned *evicted);
/* for vdp2_planes.c: colour RAM entries [0, n) kept from VDP1's palette banks (0: none); an 8bpp texture's pixels at a
 * sprite priority register (0 the default, over the planes); textures drawn first every frame, under everything */
void rsat_cram_reserve(int entries);
struct RTex;
void rsat_tex_priority(struct RTex *t, int reg);
void rsat_set_backdrops(struct RTex **t, const int *x, const int *y, int n, bool clear_framebuffer);

/* vdp2_planes.c: a level's tile layers as VDP2 planes (render.h r_layer); once a frame, before VDP1's list goes */
void sat_planes_frame(int screen_w);

/* input_sat.c */
bool sat_reset_combo(void);           /* A+B+C+Start held on pad 1 */
