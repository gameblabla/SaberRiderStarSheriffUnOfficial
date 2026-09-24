#pragma once
/* libyaul ships an empty <math.h>: the float functions the core uses, implemented in platform/saturn/libc_sat.c
 * (soft-float on the SH-2, which has no FPU; the hot paths use src/fx.h fixed point instead). */
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#define HUGE_VALF INFINITY
#define M_PI      3.14159265358979323846
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
long  lroundf(float x);
float fabsf(float x);
float fminf(float a, float b);
float fmaxf(float a, float b);
float fmodf(float x, float y);
float remainderf(float x, float y);
float copysignf(float x, float y);
float sqrtf(float x);
float hypotf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);

double floor(double x);
double ceil(double x);
double fabs(double x);
double sqrt(double x);
double fmod(double x, double y);
