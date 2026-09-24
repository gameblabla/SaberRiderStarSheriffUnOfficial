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
void cd_sat_stats(unsigned long *reads, unsigned long *bytes);

/* log_sat.c: stdout / stderr -> the RAM log ring the harness reads */
void log_sat_write(const char *s, size_t n);

/* plat_sat.c: timing from the master SH-2's free-running timer (FRT, clock / 32) */
void     sat_timer_init(void);
uint32_t sat_timer_us(void);          /* microseconds since boot (wraps after ~71 minutes) */
uint32_t sat_vblanks(void);           /* vertical blanks since boot */
void     sat_vblank_tick(void);       /* from the vblank-out handler */

/* input_sat.c */
bool sat_reset_combo(void);           /* A+B+C+Start held on pad 1 */
