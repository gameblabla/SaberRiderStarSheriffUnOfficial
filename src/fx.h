#pragma once
/* Fixed-point arithmetic for the whole game (every platform runs the same arithmetic; plan.md 8.2).
 *
 *   fx       signed 16.16: -32768 .. 32767.99998, steps of 1/65536 (world coordinates, speeds, offsets)
 *   fx_ang   a binary angle: 65536 = one turn (wraps by itself as a uint16_t; any int works, only the low 16 bits count)
 *
 * The consoles without an FPU (the Saturn's SH-2) do these with integer instructions only: a multiply is one 32x32->64
 * multiply (dmuls.l), a division runs on the SH-2's divider unit (fx.c), sines come from a table, square roots from an
 * integer square root. On the PC and the Dreamcast they are as fast as the floats they replace and give the same
 * results, bit for bit, as on the Saturn.
 *
 * Conversions from float round to nearest; FX(c) turns a constant at compile time. tools/fx_test.c checks this
 * module against libm (make test). */
#include <stdint.h>
#include <stdbool.h>

typedef int32_t fx;
typedef int32_t fx_ang;

#define FX_SHIFT 16
#define FX_ONE   ((fx)0x10000)
#define FX_HALF  ((fx)0x8000)
#define FX_MAX   ((fx)0x7FFFFFFF)
#define FX_MIN   ((fx)INT32_MIN)
/* a constant: FX(1.5), FX(-0.25) (rounded to nearest) */
#define FX(c)    ((fx)((c) * 65536.0 + ((c) >= 0 ? 0.5 : -0.5)))
#define FX_ANG_TURN 65536
/* a constant angle in degrees: FX_DEG(90) == 16384 */
#define FX_DEG(d) ((fx_ang)((d) * (65536.0 / 360.0) + ((d) >= 0 ? 0.5 : -0.5)))

static inline fx    fx_from_int(int i)     { return (fx)((uint32_t)i << FX_SHIFT); }
#ifndef FX_NO_FLOAT   /* the Saturn build defines it: no float anywhere (host tools and the PC/DC backends may convert) */
static inline fx    fx_from_float(float f) { return (fx)(f * 65536.0f + (f >= 0 ? 0.5f : -0.5f)); }
static inline float fx_to_float(fx a)      { return (float)a * (1.0f / 65536.0f); }
#endif
/* an IEEE single given as its bits (the demo's data files hold floats) to fx, rounded to nearest (halves away from
 * zero, like fx_from_float), saturating at FX_MAX / FX_MIN (the level data's +-100000 "off screen" waypoints) */
fx fx_from_f32bits(uint32_t bits);

/* The game steps at a fixed 60 Hz. As a duration (timers counting down, clocks counting up) one step is FX_DT: 1/60
 * rounded up, as float's 1/60 (0.016666668) is, so a timer of n/60 s runs out on the nth step like the float code's.
 * A rate times the step (a velocity into a displacement, an acceleration into a velocity) is v/60 exactly (to 1/65536):
 * fx_mul_dt. Any other dt (a scaled or zero step) is an ordinary product. */
#define FX_DT ((fx)1093)
static inline fx fx_mul_dt(fx v, fx dt)
{
    if (dt == FX_DT) return (fx)(((int64_t)v * 71582788 + 0x80000000) >> 32);   /* 2^32 / 60 */
    return (fx)(((int64_t)v * dt) >> 16);
}

/* to an integer: toward -infinity, toward +infinity, to nearest (halves up) */
static inline int fx_floor(fx a) { return a >> FX_SHIFT; }
static inline int fx_ceil(fx a)  { return (int)((a + 0xFFFF) >> FX_SHIFT); }
static inline int fx_round(fx a) { return (int)((a + FX_HALF) >> FX_SHIFT); }
static inline fx  fx_frac(fx a)  { return a & 0xFFFF; }
static inline fx  fx_abs(fx a)   { return a < 0 ? -a : a; }
static inline fx  fx_min(fx a, fx b) { return a < b ? a : b; }
static inline fx  fx_max(fx a, fx b) { return a > b ? a : b; }
static inline fx  fx_clamp(fx a, fx lo, fx hi) { return a < lo ? lo : a > hi ? hi : a; }

/* a * b, rounded toward -infinity like the shift */
static inline fx fx_mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> FX_SHIFT); }
/* a * b / c without the intermediate overflowing (c != 0) */
fx fx_muldiv(fx a, fx b, fx c);
/* a / b, truncated toward zero; b == 0 gives FX_MAX / FX_MIN by a's sign */
fx fx_div(fx a, fx b);

/* square roots (a <= 0 gives 0) and the length of (x, y) without overflow for any fx x, y (up to FX_MAX) */
fx fx_sqrt(fx a);
fx fx_hypot(fx x, fx y);
uint32_t fx_isqrt64(uint64_t n);   /* floor(sqrt(n)) */

/* trigonometry on binary angles: a quarter-wave table with linear interpolation (error < 1.6e-5) */
fx fx_sin(fx_ang a);
static inline fx fx_cos(fx_ang a) { return fx_sin(a + FX_ANG_TURN / 4); }
/* the angle of (x, y), 0 along +x, a quarter turn along +y (atan2 in binary angles, -32768 .. 32767); error < 0.01 degree */
fx_ang fx_atan2(fx y, fx x);

/* text: v with `dec` decimals (0..4, rounded half away from zero) into buf (at least 16 bytes); returns buf. FXS(v, d)
 * for a printf argument. */
char *fx_fmt(char *buf, fx v, int dec);
#define FXS(v, dec) fx_fmt((char[16]){ 0 }, (v), (dec))
/* the reverse: a decimal number ("-12.5") to fx; *end past it (strtod without float) */
fx fx_parse(const char *s, const char **end);

/* degrees (fx) <-> binary angles */
static inline fx_ang fx_ang_from_deg(fx d) { return (fx_ang)(((int64_t)d * 11930465 + ((int64_t)1 << 31)) >> 32); }   /* / 360 */
static inline fx     fx_deg_from_ang(fx_ang a) { return (fx)((int16_t)a * 360); }   /* -180 .. 180 */

/* radians <-> binary angles (for code whose angles are in radians) */
static inline fx_ang fx_ang_from_rad(fx r) { return (fx_ang)(((int64_t)r * 683565276 + ((int64_t)1 << 31)) >> 32); }   /* / 2pi */
static inline fx     fx_rad_from_ang(fx_ang a) { return (fx)(((int64_t)(int16_t)a * 411775 + 0x8000) >> 16); }   /* * 2pi, -pi .. pi */
#ifndef FX_NO_FLOAT
static inline fx_ang fx_ang_from_radf(float r) { return (fx_ang)(int32_t)(r * (65536.0f / 6.28318530718f) + (r >= 0 ? 0.5f : -0.5f)); }
#endif
