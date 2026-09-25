#pragma once
/* Shared between the Saturn platform files (src/platform/saturn). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* the screen: 320x224 (4:3, NORMAL_A) or 352x224 (wide, NORMAL_B), 224 lines, non-interlaced (plan 3) */
#define SAT_SCREEN_H 224
#define SAT_WIDE_W   352

/* cd_sat.c: the ISO's file list (fopen reads the CD through it) */
void *lw_malloc(size_t n);
void *lw_memalign(size_t align, size_t n);

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

void rsat_bench(void);   /* SABER_RBENCH */
/* video_sat.c: a w x h 16bpp surface in VDP1 memory, written by hook(ud, pixels, pitch in pixels) once a frame while VDP1
 * is idle; drawn by rsat_video_draw (a record, like any draw) */
bool rsat_video_open(int w, int h, void (*hook)(void *ud, volatile uint16_t *px, int pitch), void *ud);
void rsat_video_close(void);
void rsat_video_preload(void);             /* load movie SCSP driver during boot */
bool rsat_sound_driver_init(void);          /* shared PoneSound driver for game + movie audio */

/* pcm_sat.c: one PCM stream on the SCSP (a slot looping over a ring in sound RAM, fed by the SH-2; no 68000 driver) */
void     pcm_sat_init(void);
bool     pcm_sat_start(int rate, const int16_t *lead, int lead_samples);   /* 16-bit mono, big-endian (the SH-2's own) */
int      pcm_sat_space(void);                  /* samples that can be written now without overtaking the playback */
void     pcm_sat_write(const int16_t *s, int n);   /* the next n samples */
uint32_t pcm_sat_played(void);                 /* samples played since the start (the video's clock) */
void     pcm_sat_finish(const int16_t *tail, int n);   /* the rest of the stream: plays out, then the slot stops */
void     pcm_sat_stop(void);
void     pcm_sat_poll(void);                   /* once a frame: feeds a finishing stream, stops it at its end */

/* cd_sat.c: bytes a sequential read of f can take right now without waiting for the drive */
#include <stdio.h>
size_t   cd_sat_available(FILE *f);
void rsat_timing(uint32_t *planes_us, uint32_t *vdp1_wait_us, uint32_t *put_us, uint32_t *frames);   /* since the last call */

/* vdp2_planes.c: a level's tile layers as VDP2 planes (render.h r_layer); once a frame, before VDP1's list goes */
void sat_planes_frame(int screen_w, bool delayed);
int  sat_planes_depth_reg(uint32_t level, int layer);   /* a level layer's sprite priority register (0 the front) */
uint32_t sat_planes_level(void);                         /* the level whose planes are loaded (0 none) */
bool sat_floor_visible(void);

/* input_sat.c */
bool sat_reset_combo(void);           /* A+B+C+Start held on pad 1 */
