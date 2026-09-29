#pragma once
/* <math.h> for the WASM port. The core only ever calls these through real.h's r_* wrappers (float), but
 * r_fmt / r_fmt_g format a double and stb_vorbis (the music decoder) wants the double set, so both are here. */
#include <stdint.h>

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))
#define HUGE_VAL    (__builtin_huge_valf())
#define HUGE_VALF   INFINITY
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

float  floorf(float x);
float  ceilf(float x);
float  truncf(float x);
float  roundf(float x);
float  fabsf(float x);
float  fmodf(float x, float y);
float  remainderf(float x, float y);
float  copysignf(float x, float y);
float  fminf(float a, float b);
float  fmaxf(float a, float b);
float  sqrtf(float x);
float  hypotf(float x, float y);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  atanf(float x);
float  atan2f(float y, float x);
float  asinf(float x);
float  acosf(float x);
float  expf(float x);
float  logf(float x);
float  log2f(float x);
float  log10f(float x);
float  powf(float x, float y);
float  ldexpf(float x, int e);
int    ilogbf(float x);
float  fdimf(float a, float b);

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fabs(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double copysign(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
double sqrt(double x);
double hypot(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double ldexp(double x, int e);
int    ilogb(double x);
double fdim(double a, double b);
