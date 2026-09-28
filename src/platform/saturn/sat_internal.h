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
/* CD-DA music on the same drive: a disc track (2..), looped or once; update once a frame (it resumes the music after
 * data reads took the drive) */
bool cd_sat_cdda_play(int track, bool loop);
void cd_sat_cdda_track_length(int track, uint32_t sectors);  /* playable sectors after INDEX 01 */
void cd_sat_cdda_stop(void);
void cd_sat_cdda_pause(bool pause);
void cd_sat_cdda_update(void);

/* log_sat.c: stdout / stderr -> the RAM log ring the harness reads */
void log_sat_write(const char *s, size_t n);

/* plat_sat.c: timing from the master SH-2's free-running timer (FRT, clock / 32) */
void     sat_timer_init(void);
uint32_t sat_timer_us(void);          /* microseconds since boot (wraps after ~71 minutes) */
void     sat_busy_wait_us(uint32_t us, bool mode352);   /* with the interrupts off (the FRT counted by polling) */
void     sat_timer_clock(bool mode352, uint32_t now_us);   /* the system clock changed to the 352 (28.64 MHz) or 320 (26.87 MHz) modes' */
uint32_t sat_vblanks(void);           /* vertical blanks since boot */
void     sat_vblank_tick(void);       /* from the vblank-out handler */

/* render_sat.c: VDP1 */
typedef struct Ren Ren;
Ren *rsat_renderer(void);
void rsat_init(void);
void rsat_set_screen(int w, int h);
void rsat_set_mode(bool wide);   /* 352x224 (true) or 320x224: the TV mode and system clock, re-initialising what that resets */
void rsat_frame_begin(void);
void rsat_frame_end(void);
int  rsat_prims(void);
void rsat_stats(unsigned *parts_resident, unsigned *vram_used, unsigned *uploads, unsigned *evicted);
/* for vdp2_planes.c: colour RAM entries [0, n) kept from VDP1's palette banks (0: none); an 8bpp texture's pixels at a
 * sprite priority register (0 the default, over the planes); textures drawn first every frame, under everything */
void rsat_cram_reserve(int entries);
void rsat_cram_put_be(int first, const uint8_t *be, int n);   /* n big-endian RGB555 colours from entry first, at the vblank */
void rsat_cram_flush(void);                                   /* the vblank-in handler: the colours written since, into colour RAM */
struct RTex;
void rsat_tex_priority(struct RTex *t, int reg);
void rsat_set_backdrops(struct RTex **t, const int *x, const int *y, int n, bool clear_framebuffer);
void rsat_backdrops_dy(int dy);   /* the frame being recorded: the backdrops drawn dy higher (with the planes' scroll) */

void rsat_bench(void);   /* SABER_RBENCH */
/* video_sat.c: a w x h 16bpp surface in VDP1 memory, written by hook(ud, pixels, pitch in pixels) once a frame while VDP1
 * is idle; drawn by rsat_video_draw (a record, like any draw) */
bool rsat_video_open(int w, int h, void (*hook)(void *ud, volatile uint16_t *px, int pitch), void *ud);
void rsat_video_close(void);
/* aud_sat.c: a clip has the SCSP from its start to its close (the game's sound driver stops, then starts again);
 * music: the clip doesn't read the disc (played from RAM), the CD-DA music plays on under it */
void aud_movie_begin(bool music);
void aud_movie_end(void);
/* aud_sat.c: the system clock change resets the SCSP (the driver goes around it, the sound RAM stays) */
void aud_clock_change(bool begin);

/* cd_sat.c: bytes a sequential read of f can take right now without waiting for the drive */
#include <stdio.h>
size_t   cd_sat_available(FILE *f);
void rsat_timing(uint32_t *planes_us, uint32_t *vdp1_wait_us, uint32_t *put_us, uint32_t *frames);   /* since the last call */

/* vdp2_planes.c: a level's tile layers as VDP2 planes (render.h r_layer); once a frame, before VDP1's list goes */
void sat_planes_frame(int screen_w);
void sat_planes_shown(void);                             /* VDP1's frame changes now (the vblank-in): its planes' scroll */
int  sat_planes_depth_reg(uint32_t level, int layer);   /* a level layer's sprite priority register (0 the front) */
uint32_t sat_planes_level(void);                         /* the level whose planes are loaded (0 none) */
bool sat_floor_visible(void);

/* input_sat.c */
bool sat_reset_combo(void);           /* A+B+C+Start held on pad 1 */

/* smpc_peripheral_sat.c: the SMPC runs one command at a time; the vblank's INTBACK only when sat_smpc_intback_ok, and
 * the game's own SMPC commands (SSHON / SNDON..., the clock change) between sat_smpc_lock / sat_smpc_unlock (nests;
 * call it with the interrupts on where possible: the INTBACK in flight ends through its interrupt) */
bool sat_smpc_intback_ok(void);
void sat_smpc_lock(void);
void sat_smpc_unlock(void);

/* diag_sat.c (make DIAG=1): the log on screen until the main loop runs, and when it stalls */
#ifdef SAT_DIAG
void sat_diag_vblank(void);   /* from the vblank-in handler */
void sat_diag_alive(void);    /* the main loop finished a frame */
void sat_diag_stage(unsigned s);   /* a boot stage reached: its colour on the whole screen (diag_sat.c) */
#else
static inline void sat_diag_stage(unsigned s) { (void)s; }
static inline void sat_diag_vblank(void) { }
static inline void sat_diag_alive(void) { }
#endif
