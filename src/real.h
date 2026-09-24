#pragma once
/* The game's scalar type, `real`: float on the PC and the Dreamcast (the demo's own arithmetic, unchanged), 16.16 fixed
 * point (fx.h) on the Saturn, whose SH-2s have no FPU: no soft-float there at all. REAL_FIXED selects fixed point;
 * PLAT_SATURN implies it, and the headless host build takes it too (make -f Makefile.headless FIXED=1) so both
 * arithmetics can be compared on the PC (tools/regress.py).
 *
 * One source for both: + - unary minus, comparisons, real * int and real / int are plain C either way. Everything else
 * goes through these helpers, which are the float expressions the code always had (so the float build is bit for bit
 * what it was) and their fixed-point equivalents:
 *   R(c)                 a constant (R(0.5), R(-290))
 *   r_int(i)             an int as a real;           r_floor / r_ceil / r_trunc / r_round: a real to an int
 *   r_mul r_div r_muldiv products and quotients of two reals (a * b / c without overflow in fixed point)
 *   r_mul_dt(v, dt)      a rate over one step (dt is R_DT, the fixed 60 Hz step; exact v / 60 in fixed point)
 *   r_sin r_cos r_atan2  radians, like sinf / atan2f;  r_sqrt r_hypot r_abs r_min r_max r_fmod r_floorr r_remainder
 *   r_len3(x, y, z)      a 3D vector's length; r_within2 / r_within3(d.., r): the vector shorter than r
 *   r_bits(u)            the demo's data files' IEEE floats, from their bits
 *   r_fmt / RS(v, d)     text with d decimals (printf("%s", RS(x, 2)) for "%.2f"); RSG(v) for "%g"
 *   r_ms(ms), r_milli(v) milliseconds (an int, a real) as seconds (* 0.001f)
 * Angles for r_tex_rot are `rdeg`: degrees, a double on the float build (as SDL takes them), fx otherwise.
 * In fixed point a real holds -32768 .. 32767.99998: squares of distances and the like go through r_hypot or 64 bits. */
#include "fx.h"
#include <stdint.h>
#if defined(PLAT_SATURN) && !defined(REAL_FIXED)
#define REAL_FIXED 1
#endif

#ifdef REAL_FIXED
typedef fx real;
typedef fx rdeg;
#define R(c)     FX(c)
#define R_DT     FX_DT
#define R_MAX    FX_MAX
static inline real r_int(int i) { return fx_from_int(i); }
static inline real r_mul(real a, real b) { return fx_mul(a, b); }
static inline real r_div(real a, real b) { return fx_div(a, b); }
static inline real r_muldiv(real a, real b, real c) { return fx_muldiv(a, b, c); }
static inline real r_mul_dt(real v, real dt) { return fx_mul_dt(v, dt); }
static inline int  r_floor(real a) { return fx_floor(a); }
static inline int  r_ceil(real a) { return fx_ceil(a); }
static inline int  r_trunc(real a) { return a >= 0 ? a >> FX_SHIFT : -(int)((0u - (uint32_t)a) >> FX_SHIFT); }
static inline int  r_round(real a) { return a >= 0 ? (a + FX_HALF) >> FX_SHIFT : -(int)(((0u - (uint32_t)a) + FX_HALF) >> FX_SHIFT); }   /* roundf: halves away from 0 */
static inline real r_floorr(real a) { return a & ~0xFFFF; }
static inline real r_abs(real a) { return fx_abs(a); }
static inline real r_min(real a, real b) { return fx_min(a, b); }
static inline real r_max(real a, real b) { return fx_max(a, b); }
static inline real r_sqrt(real a) { return fx_sqrt(a); }
static inline real r_hypot(real x, real y) { return fx_hypot(x, y); }
static inline real r_sin(real rad) { return fx_sin(fx_ang_from_rad(rad)); }
static inline real r_cos(real rad) { return fx_cos(fx_ang_from_rad(rad)); }
static inline real r_atan2(real y, real x) { return fx_rad_from_ang(fx_atan2(y, x)); }
static inline real r_fmod(real a, real b) { return b ? a % b : 0; }   /* the sign of a, like fmodf */
static inline real r_remainder(real a, real b) { return b ? a - r_round(fx_div(a, b)) * b : 0; }   /* a - nearest multiple of b, like remainderf (halves away from 0, not to even) */
/* sqrt(x^2 + y^2 + z^2) without the squares overflowing */
static inline real r_len3(real x, real y, real z)
{
    uint32_t r = fx_isqrt64((uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)y * y) + (uint64_t)((int64_t)z * z));
    return r > (uint32_t)FX_MAX ? FX_MAX : (real)r;
}
static inline real r_len2(real x, real y)
{
    uint32_t r = fx_isqrt64((uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)y * y));
    return r > (uint32_t)FX_MAX ? FX_MAX : (real)r;
}
/* |(dx, dy)| < r, |(dx, dy, dz)| < r: squared distances in 64 bits */
static inline bool r_within2(real dx, real dy, real r) { return (int64_t)dx * dx + (int64_t)dy * dy < (int64_t)r * r; }
static inline bool r_within3(real dx, real dy, real dz, real r) { return (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz < (int64_t)r * r; }
static inline real r_bits(uint32_t b) { return fx_from_f32bits(b); }
static inline char *r_fmt(char *buf, real v, int dec) { return fx_fmt(buf, v, dec); }
static inline char *r_fmt_g(char *buf, real v) { return fx_fmt(buf, v, 4); }
static inline real r_ms(int ms) { return (real)(((int64_t)ms * 65536 + (ms >= 0 ? 500 : -500)) / 1000); }   /* milliseconds to seconds */
static inline real r_milli(real v) { return v / 1000; }
static inline real r_parse(const char *s, const char **end) { return fx_parse(s, end); }
/* radians (real) to r_tex_rot's degrees */
static inline rdeg r_deg(real rad) { return fx_muldiv(rad, FX(180), FX(3.14159265)); }
#else
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef float real;
typedef double rdeg;
#define R(c)     ((float)(c))
#define R_DT     ((float)(1.0 / 60.0))
#define R_MAX    3.0e38f
static inline real r_int(int i) { return (float)i; }
static inline real r_mul(real a, real b) { return a * b; }
static inline real r_div(real a, real b) { return a / b; }
static inline real r_muldiv(real a, real b, real c) { return a * b / c; }
static inline real r_mul_dt(real v, real dt) { return v * dt; }
static inline int  r_floor(real a) { return (int)floorf(a); }
static inline int  r_ceil(real a) { return (int)ceilf(a); }
static inline int  r_trunc(real a) { return (int)a; }
static inline int  r_round(real a) { return (int)roundf(a); }
static inline real r_floorr(real a) { return floorf(a); }
static inline real r_abs(real a) { return fabsf(a); }
static inline real r_min(real a, real b) { return fminf(a, b); }
static inline real r_max(real a, real b) { return fmaxf(a, b); }
static inline real r_sqrt(real a) { return sqrtf(a); }
static inline real r_hypot(real x, real y) { return hypotf(x, y); }
static inline real r_sin(real rad) { return sinf(rad); }
static inline real r_cos(real rad) { return cosf(rad); }
static inline real r_atan2(real y, real x) { return atan2f(y, x); }
static inline real r_fmod(real a, real b) { return fmodf(a, b); }
static inline real r_remainder(real a, real b) { return remainderf(a, b); }
static inline real r_len2(real x, real y) { return sqrtf(x * x + y * y); }
static inline real r_len3(real x, real y, real z) { return sqrtf(x * x + y * y + z * z); }
static inline bool r_within2(real dx, real dy, real r) { return dx * dx + dy * dy < r * r; }
static inline bool r_within3(real dx, real dy, real dz, real r) { return dx * dx + dy * dy + dz * dz < r * r; }
static inline real r_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline char *r_fmt(char *buf, real v, int dec) { snprintf(buf, 24, "%.*f", dec, (double)v); return buf; }
static inline char *r_fmt_g(char *buf, real v) { snprintf(buf, 24, "%g", (double)v); return buf; }
static inline real r_ms(int ms) { return ms * 0.001f; }
static inline real r_milli(real v) { return v * 0.001f; }
static inline real r_parse(const char *s, const char **end) { char *e; float f = strtof(s, &e); if (end) *end = e; return f; }
static inline rdeg r_deg(real rad) { return rad * 180.0 / 3.14159265; }
#endif

#define RS(v, dec) r_fmt((char[24]){ 0 }, (v), (dec))
#define RSG(v)     r_fmt_g((char[24]){ 0 }, (v))   /* "%g" */
