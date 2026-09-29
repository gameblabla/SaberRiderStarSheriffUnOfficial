/* The maths for wasm32-unknown-unknown: no libm in a freestanding module, and neither the game nor the music
 * decoder may be left without it.
 *
 * The core's r_* wrappers (real.h) call these in float; stb_vorbis, which decodes the packs' MUPS music, calls
 * them in double. The transcendentals are the standard argument-reduction-then-polynomial kind, with the
 * polynomials taken to the precision each type needs (float uses a shortened kernel, double the full one), so a
 * run's physics matches the PC build's to well under a pixel. The exact operations wasm has instructions for
 * (f32.floor, f32.ceil, f32.trunc, f32.abs, f32.sqrt, f32.nearest) go through the builtins so the compiler
 * emits the instruction and not a call. */
#include <stdint.h>
#include <string.h>
/* Its own header, so the definitions below match the declarations the port is built against. The angle brackets
 * put it on the include path (the port's own include directory, which the build and the host test both add first),
 * which also works when this file is #included from elsewhere (tools/wasm/libm_test.c). */
#include <math.h>

#define TOF(x) ((float)(x))
#define TOD(x) ((double)(x))

/* ------------------------------------------------------------------ rounding */
float  floorf(float x)  { return __builtin_floorf(x); }
float  ceilf(float x)   { return __builtin_ceilf(x); }
float  truncf(float x)  { return __builtin_truncf(x); }
float  fabsf(float x)   { return __builtin_fabsf(x); }
float  sqrtf(float x)   { return __builtin_sqrtf(x); }
double floor(double x)  { return __builtin_floor(x); }
double ceil(double x)   { return __builtin_ceil(x); }
double trunc(double x)  { return __builtin_trunc(x); }
double fabs(double x)   { return __builtin_fabs(x); }
double sqrt(double x)   { return __builtin_sqrt(x); }

/* roundf / round: halves away from zero, which is what r_round promises and what r_remainder's fixed-point twin
 * does (the wasm f32.nearest instruction rounds halves to even, so it cannot be used) */
float roundf(float x)
{
    if (!__builtin_isfinite(x)) return x;
    float t = __builtin_truncf(x), f = x - t;         /* f is in [0,1), or (-1,0) for a negative x */
    /* halves away from zero, which is what r_round promises: the increment takes x's sign, not the truncation's */
    return t + __builtin_copysignf(f >= 0.5f || f <= -0.5f ? 1.0f : 0.0f, x);
}
double round(double x)
{
    if (!__builtin_isfinite(x)) return x;
    double t = __builtin_trunc(x), f = x - t;
    return t + __builtin_copysign(f >= 0.5 || f <= -0.5 ? 1.0 : 0.0, x);
}

float fminf(float a, float b)
{
    if (__builtin_isnan(a)) return b;
    if (__builtin_isnan(b)) return a;
    return a < b ? a : b;
}
float fmaxf(float a, float b)
{
    if (__builtin_isnan(a)) return b;
    if (__builtin_isnan(b)) return a;
    return a > b ? a : b;
}
double fmin(double a, double b) { return __builtin_isnan(a) ? b : __builtin_isnan(b) ? a : (a < b ? a : b); }
double fmax(double a, double b) { return __builtin_isnan(a) ? b : __builtin_isnan(b) ? a : (a > b ? a : b); }
float  fdimf(float a, float b) { return a > b ? a - b : 0.0f; }
double fdim(double a, double b) { return a > b ? a - b : 0.0; }
float  copysignf(float x, float y) { return __builtin_copysignf(x, y); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }

/* fmod / remainder, without a libm: the slow way, by repeated subtraction of the scaled divisor. The core uses
 * these for angles and a timer, never in a loop over a long range, so the simple form is the right trade. */
float fmodf(float x, float y)
{
    if (!__builtin_isfinite(x) || !__builtin_isfinite(y) || y == 0.0f) return (float)NAN;
    int neg = __builtin_signbit(x);
    x = __builtin_fabsf(x);
    y = __builtin_fabsf(y);
    if (x < y) return __builtin_copysignf(x, neg ? -1.0f : 1.0f);
    /* fmod is the remainder of the binary long division: start at the largest power of two that keeps y*scale
     * inside x and halve it, subtracting y*scale whenever it still fits. That is exact for both operands (each
     * step is a subtraction of a multiple of y, so no rounding accumulates) and bounded by the exponent
     * difference, at most 254 for a float. */
    /* scale is a power of two no smaller than 1, so y*scale is always an integer multiple of y and each
     * subtraction is exact; the remainder left is in [0, y) and is the fmod. */
    float scale = 1.0f;
    while (scale <= x / (y * 2.0f)) scale *= 2.0f;   /* the largest 2^k with y*scale <= x/2 */
    while (scale >= 1.0f) {
        if (y * scale <= x) x -= y * scale;
        scale *= 0.5f;
    }
    return __builtin_copysignf(x, neg ? -1.0f : 1.0f);
}
float remainderf(float x, float y)
{
    if (y == 0.0f || !__builtin_isfinite(x) || !__builtin_isfinite(y)) return (float)NAN;
    float q = x / y, n = roundf(q);
    return x - n * y;
}
double fmod(double x, double y)
{
    if (!__builtin_isfinite(x) || !__builtin_isfinite(y) || y == 0.0) return (double)NAN;
    int neg = __builtin_signbit(x);
    x = __builtin_fabs(x);
    y = __builtin_fabs(y);
    if (x < y) return __builtin_copysign(x, neg ? -1.0 : 1.0);
    double scale = 1.0;
    while (scale <= x / (y * 2.0)) scale *= 2.0;
    while (scale >= 1.0) {
        if (y * scale <= x) x -= y * scale;
        scale *= 0.5;
    }
    return __builtin_copysign(x, neg ? -1.0 : 1.0);
}
double remainder(double x, double y)
{
    if (y == 0.0 || !__builtin_isfinite(x) || !__builtin_isfinite(y)) return (double)NAN;
    double n = round(x / y);
    return x - n * y;
}

/* ilogb: the exponent of a normal, by hand (the two fmod implementations want it to size their reduction) */
int ilogbf(float x)
{
    if (x == 0.0f || !__builtin_isfinite(x)) return 0;
    union { float f; uint32_t u; } s;
    s.f = x;
    int e = (int)((s.u >> 23) & 0xff) - 127;
    /* a subnormal has no exponent field: scale it into range first */
    if (e == -127) { s.f = x * 16777216.0f; e = (int)((s.u >> 23) & 0xff) - 127 - 24; }
    return e;
}
int ilogb(double x)
{
    if (x == 0.0 || !__builtin_isfinite(x)) return 0;
    union { double d; uint64_t u; } s;
    s.d = x;
    int e = (int)((s.u >> 52) & 0x7ff) - 1023;
    if (e == -1023) { s.d = x * 9007199254740992.0; e = (int)((s.u >> 52) & 0x7ff) - 1023 - 53; }
    return e;
}

float  hypotf(float x, float y) { return sqrtf(x * x + y * y); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }

/* ------------------------------------------------------------------ sin / cos
 * Range reduction to [-pi/4, pi/4] by quadrant, then a Taylor-shaped kernel. float uses a degree-7 odd kernel
 * (about 1e-7 relative), double a degree-13 one (about 1e-16). sin and cos share the reduced value, which is what
 * r_sin and r_cos want: they always come as a pair. */
#define F_PI   3.14159265358979323846f
#define D_PI   3.14159265358979323846

static float sin_kernel_f(float x)
{
    float x2 = x * x;
    /* 1 - x^2/6 + x^4/120 - x^6/5040 */
    return x * (1.0f + x2 * (-1.66666666666666657e-1f + x2 * (8.33333333333333333e-3f + x2 * -1.98412698412698413e-4f)));
}
static float cos_kernel_f(float x)
{
    float x2 = x * x;
    /* 1 - x^2/2 + x^4/24 - x^6/720 + x^8/40320 */
    return 1.0f + x2 * (-5.00000000000000000e-1f + x2 * (4.16666666666666644e-2f +
               x2 * (-1.38888888888888894e-3f + x2 * 2.48015873015873016e-5f)));
}
static double sin_kernel_d(double x)
{
    /* sin x = x * sum (-1)^k x^2k / (2k+1)!, out to x^17 */
    static const double S[] = { -1.66666666666666657415e-1,  8.33333333333333321769e-3, -1.98412698412698412554e-4,
                                 2.75573192239858925126e-6, -2.50521083854417187751e-8,  1.60590438368216145994e-10 };
    double x2 = x * x, p = 0.0;
    for (int i = (int)(sizeof S / sizeof *S) - 1; i >= 0; i--) p = p * x2 + S[i];
    return x * (1.0 + x2 * p);
}
static double cos_kernel_d(double x)
{
    /* Horner, so the nesting cannot run away: cos x = sum (-1)^k x^2k / (2k)! */
    static const double C[] = { -5.00000000000000000000e-1,  4.16666666666666643537e-2, -1.38888888888888894189e-3,
                                 2.48015873015873015658e-5, -2.75573192239858906526e-7,  2.08767569878680989792e-9,
                                -1.14707455977297247139e-11 };
    double x2 = x * x, p = 0.0;
    for (int i = (int)(sizeof C / sizeof *C) - 1; i >= 0; i--) p = p * x2 + C[i];
    return 1.0 + x2 * p;
}

/* Reduce to r in [-pi/4, pi/4] and a quadrant k in 0..3, such that
 *      x = k*(pi/2) + r   and   sin(x) / cos(x) come off the two kernels by the quadrant.
 * The subtraction is two-part (Cody-Waite) against a split pi/2, so it stays exact over the argument sizes the
 * game uses (angles, a few thousand radians at most). A negative x must give a negative k, so the quadrant is
 * taken from the quotient's sign and only the magnitude is folded - folding first and then taking the quadrant
 * off the folded value is what gets a negative argument wrong. */
static void reduce_f(float x, int *k, float *r)
{
    if (!__builtin_isfinite(x)) { *k = 0; *r = (float)NAN; return; }
    /* n = the whole number of turns, and q = the quarter turn within it. Both come from the *unfolded* quotient:
     * folding first and then subtracting only the quarter is what leaves whole turns in the remainder, which is
     * the difference between a sine and a large number. The quotient is held in a double so that x/(2pi) keeps
     * its integer part exactly for the argument sizes the game uses (angles, tens of radians). */
    double qd = (double)x * (1.0 / 6.283185307179586);
    long n = (long)qd;                       /* truncate toward zero: whole turns, same sign as x */
    double frac = qd - (double)n;             /* [0,1) or (-1,0] */
    int neg = frac < 0.0;
    if (neg) frac = -frac;
    /* the remainder in radians, from the *fractional* part, plus the quarter the fraction lands in */
    int q = (int)(frac * 4.0 + 0.5);
    if (q > 3) q = 3;
    /* r = |frac| * 2pi - q * (pi/2), done so the quarter subtraction stays exact */
    float fr = (float)(frac * 6.283185307179586);
    float rr = fr - (float)q * 1.57079625129699707f;
    rr -= (float)q * 3.5772210189059739e-8f;
    if (rr > F_PI * 0.25f) { rr -= F_PI * 0.5f; q = (q + 1) & 3; }
    else if (rr < -F_PI * 0.25f) { rr += F_PI * 0.5f; q = (q + 3) & 3; }
    (void)n;
    *k = neg ? (4 - q) & 3 : q;
    *r = neg ? -rr : rr;
}
static void reduce_d(double x, int *k, double *r)
{
    if (!__builtin_isfinite(x)) { *k = 0; *r = (double)NAN; return; }
    double qd = x * (1.0 / 6.283185307179586);
    long n = (long)qd;
    double frac = qd - (double)n;
    int neg = frac < 0.0;
    if (neg) frac = -frac;
    int q = (int)(frac * 4.0 + 0.5);
    if (q > 3) q = 3;
    double rr = frac * 6.283185307179586 - (double)q * 1.57079632673412561417;
    rr -= (double)q * 6.07710050650619224932e-11;
    if (rr > D_PI * 0.25) { rr -= D_PI * 0.5; q = (q + 1) & 3; }
    else if (rr < -D_PI * 0.25) { rr += D_PI * 0.5; q = (q + 3) & 3; }
    (void)n;
    *k = neg ? (4 - q) & 3 : q;
    *r = neg ? -rr : rr;
}

float sinf(float x)
{
    int k;
    float r;
    reduce_f(x, &k, &r);
    switch (k) {
    case 0: return sin_kernel_f(r);
    case 1: return cos_kernel_f(r);
    case 2: return -sin_kernel_f(r);
    default: return -cos_kernel_f(r);
    }
}
float cosf(float x)
{
    int k;
    float r;
    reduce_f(x, &k, &r);
    switch (k) {
    case 0: return cos_kernel_f(r);
    case 1: return -sin_kernel_f(r);
    case 2: return -cos_kernel_f(r);
    default: return sin_kernel_f(r);
    }
}
double sin(double x)
{
    int k;
    double r;
    reduce_d(x, &k, &r);
    switch (k) {
    case 0: return sin_kernel_d(r);
    case 1: return cos_kernel_d(r);
    case 2: return -sin_kernel_d(r);
    default: return -cos_kernel_d(r);
    }
}
double cos(double x)
{
    int k;
    double r;
    reduce_d(x, &k, &r);
    switch (k) {
    case 0: return cos_kernel_d(r);
    case 1: return -sin_kernel_d(r);
    case 2: return -cos_kernel_d(r);
    default: return sin_kernel_d(r);
    }
}
float tanf(float x) { return sinf(x) / cosf(x); }
double tan(double x) { return sin(x) / cos(x); }

/* ------------------------------------------------------------------ atan
 * atan(x) = 2*atan(x / (1 + sqrt(1 + x^2))) applied a few times, which takes any x into a small argument
 * (each application roughly halves it), and then a plain Taylor series there, where it converges fast. The
 * reciprocal rule (atan x = pi/2 - atan(1/x)) handles |x| > 1 first, so the halving never has to work with a
 * tiny argument. Five halvings and x^9 terms give a double's full precision; the float path uses three. */
/* 1 - x^2/3 + x^4/5 - x^6/7 + x^8/9, in Horner form over x^2. The coefficients run from the highest power down
 * to the constant 1, which is the last one added - starting the accumulator at the constant instead evaluates the
 * polynomial the other way round and gives a number that is nowhere near the series. */
static const float ATAN_C[] = { 1.0f, -1.0f / 3.0f, 1.0f / 5.0f, -1.0f / 7.0f, 1.0f / 9.0f };

static float atan_kernel_f(float x)
{
    float x2 = x * x, p = ATAN_C[4];
    for (int i = 3; i >= 0; i--) p = p * x2 + ATAN_C[i];
    return p * x;
}
static double atan_kernel_d(double x)
{
    double x2 = x * x, p = ATAN_C[4];
    for (int i = 3; i >= 0; i--) p = p * x2 + (double)ATAN_C[i];
    return p * x;
}

float atanf(float x)
{
    if (!__builtin_isfinite(x)) return x > 0 ? F_PI * 0.5f : -F_PI * 0.5f;
    int neg = x < 0.0f;
    if (neg) x = -x;
    int inv = x > 1.0f;
    if (inv) x = 1.0f / x;
    float mul = 1.0f;
    for (int i = 0; i < 3 && x > 0.05f; i++) { x = x / (1.0f + sqrtf(1.0f + x * x)); mul *= 2.0f; }
    float a = atan_kernel_f(x) * mul;
    if (inv) a = F_PI * 0.5f - a;
    return neg ? -a : a;
}
double atan(double x)
{
    if (!__builtin_isfinite(x)) return x > 0 ? D_PI * 0.5 : -D_PI * 0.5;
    int neg = x < 0.0;
    if (neg) x = -x;
    int inv = x > 1.0;
    if (inv) x = 1.0 / x;
    double mul = 1.0;
    for (int i = 0; i < 5 && x > 0.05; i++) { x = x / (1.0 + sqrt(1.0 + x * x)); mul *= 2.0; }
    double a = atan_kernel_d(x) * mul;
    if (inv) a = D_PI * 0.5 - a;
    return neg ? -a : a;
}
float atan2f(float y, float x)
{
    if (x == 0.0f && y == 0.0f) return 0.0f;   /* the C library's choice, and the game's own */
    if (y == 0.0f) return x > 0.0f ? 0.0f : __builtin_copysignf(F_PI, y);   /* the sign of a zero y decides +-pi */
    if (x == 0.0f) return __builtin_copysignf(F_PI * 0.5f, y);
    float a = atanf(y / x);
    if (x > 0.0f) return a;
    return a + __builtin_copysignf(F_PI, y);
}
double atan2(double y, double x)
{
    if (x == 0.0 && y == 0.0) return 0.0;
    if (y == 0.0) return x > 0.0 ? 0.0 : __builtin_copysign(D_PI, y);
    if (x == 0.0) return __builtin_copysign(D_PI * 0.5, y);
    double a = atan(y / x);
    if (x > 0.0) return a;
    return a + __builtin_copysign(D_PI, y);
}
float asinf(float x)
{
    if (x >= 1.0f) return F_PI * 0.5f;
    if (x <= -1.0f) return -F_PI * 0.5f;
    return atanf(x / sqrtf(1.0f - x * x));
}
float acosf(float x)
{
    if (x >= 1.0f) return 0.0f;
    if (x <= -1.0f) return F_PI;
    return F_PI * 0.5f - asinf(x);
}
double asin(double x)
{
    if (x >= 1.0) return D_PI * 0.5;
    if (x <= -1.0) return -D_PI * 0.5;
    return atan(x / sqrt(1.0 - x * x));
}
double acos(double x)
{
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return D_PI;
    return D_PI * 0.5 - asin(x);
}

/* ------------------------------------------------------------------ exp / log / pow
 * exp: 2^k * exp(r) with r in [-ln2/2, ln2/2], the kernel a degree-6 Taylor. log: frexp-style, a table-free
 * atanh series on the mantissa. pow: exp(y * log(x)) with the exact cases first. */
float expf(float x)
{
    if (x != x) return x;
    if (x > 88.0f) return INFINITY;
    if (x < -87.0f) return 0.0f;
    /* k = round(x * log2(e)) picks the power of two, and r = x - k*ln2 is the remainder. The products are done in
     * two parts (Cody-Waite) against a split ln2, so r keeps its precision for the argument sizes exp sees here. */
    float k = roundf(x * 1.44269504088896341f);
    /* ln2 split so that k*ln2_hi is exact: ln2_hi keeps only the leading bits, ln2_lo the rest */
    float r = (x - k * 0.69314718246459961f) - k * 9.352417308e-9f;
    /* e^r = 1 + r + r^2/2! + ... : enough terms for |r| <= ln2/2 to a float's precision (a term 1/k! weighs
     * (ln2/2)^k/k!, and float has about 2^-24 of room, so 1/11! is the last one that still shows) */
    static const float E[] = { 1.0f, 1.0f, 0.5f, 1.66666666666666657e-1f, 4.16666666666666644e-2f,
                               8.33333333333333333e-3f, 1.38888888888888894e-3f, 1.98412698412698413e-4f,
                               2.48015873015873016e-5f, 2.75573192239858925e-6f, 2.75573192239858925e-7f };
    float p = 0.0f;
    for (int i = (int)(sizeof E / sizeof *E) - 1; i >= 0; i--) p = p * r + E[i];
    return p * ldexpf(1.0f, (int)k);                 /* 2^k exactly, no exponent field to overflow */
}
double exp(double x)
{
    if (x != x) return x;
    if (x > 709.0) return 1.0e308 * 1.0e308;
    if (x < -745.0) return 0.0;
    double k = round(x * 1.44269504088896341);
    double r = (x - k * 0.69314718036912381649) - k * 1.90821492927058770002e-10;
    /* out to 1/16!: a term 1/k! weighs (ln2/2)^k/k!, and a double has about 2^-53 of room */
    static const double E[] = { 1.0, 1.0, 0.5, 1.66666666666666657415e-1, 4.16666666666666643537e-2,
                                8.33333333333333321769e-3, 1.38888888888888894189e-3, 1.98412698412698412554e-4,
                                2.48015873015873015658e-5, 2.75573192239858925126e-6, 2.75573192239858906526e-7,
                                2.50521083854417187751e-8, 2.08767569878680989792e-9, 1.60590438368216145994e-10,
                                1.14707455977297247139e-11, 7.64716373181981647594e-13 };
    double p = 0.0;
    for (int i = (int)(sizeof E / sizeof *E) - 1; i >= 0; i--) p = p * r + E[i];
    return p * ldexp(1.0, (int)k);
}
float logf(float x)
{
    if (x != x) return x;
    if (x < 0.0f) return (float)NAN;
    if (x == 0.0f) return -INFINITY;
    if (!__builtin_isfinite(x)) return x;
    int e = (int)__builtin_ilogbf(x);
    union { float f; uint32_t u; } s;
    s.f = x;
    s.u = (s.u & 0x007fffffu) | 0x3f800000u;   /* the mantissa in [1,2) */
    /* log(m) = 2 * atanh((m-1)/(m+1)) */
    float u = (s.f - 1.0f) / (s.f + 1.0f), u2 = u * u;
    /* log(m) = 2*atanh(u), u = (m-1)/(m+1); atanh(u)/u = 1 + u^2/3 + u^4/5 + ... so the constant term is 1 */
    static const float L[] = { 1.0f, 3.33333333333333333e-1f, 2.00000000000000000e-1f, 1.42857142857142857e-1f,
                               1.11111111111111111e-1f, 9.09090909090909091e-2f, 7.69230769230769231e-2f };
    float p = 0.0f;
    for (int i = (int)(sizeof L / sizeof *L) - 1; i >= 0; i--) p = p * u2 + L[i];
    p *= u;
    /* ln2 in two parts, so e*ln2_hi is exact for the exponents a float reaches and ln2_lo finishes it */
    return (float)e * 0.69314718055994531f + 2.0f * p;
}
double log(double x)
{
    if (x != x) return x;
    if (x < 0.0) return (double)NAN;
    if (x == 0.0) return -1.0 / 0.0;
    if (!__builtin_isfinite(x)) return x;
    int e = (int)__builtin_ilogb(x);
    union { double d; uint64_t u; } s;
    s.d = x;
    s.u = (s.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double u = (s.d - 1.0) / (s.d + 1.0), u2 = u * u;
    static const double L[] = { 1.0, 3.33333333333333333333e-1, 2.00000000000000000000e-1, 1.42857142857142857143e-1,
                                1.11111111111111111111e-1, 9.09090909090909090909e-2, 7.69230769230769230769e-2,
                                6.66666666666666666667e-2, 5.88235294117647058824e-2, 5.26315789473684210526e-2,
                                4.76190476190476190476e-2 };
    double p = 0.0;
    for (int i = (int)(sizeof L / sizeof *L) - 1; i >= 0; i--) p = p * u2 + L[i];
    p *= u;
    /* ln2 split: the leading part keeps the bits e*ln2_hi needs, the tail finishes the product */
    double hi = 0.69314718036912381649, lo = 1.90821492927058770002e-10;
    return (double)e * hi + ((double)e * lo + 2.0 * p);
}
float log2f(float x) { return x <= 0.0f ? (float)(x < 0.0f ? NAN : -INFINITY) : logf(x) * 1.44269504088896341f; }
double log2(double x) { return x <= 0.0 ? (x < 0.0 ? (double)NAN : -1.0 / 0.0) : log(x) * 1.4426950408889634; }
float log10f(float x) { return x <= 0.0f ? (float)(x < 0.0f ? NAN : -INFINITY) : logf(x) * 0.43429448190325176f; }
double log10(double x) { return x <= 0.0 ? (x < 0.0 ? (double)NAN : -1.0 / 0.0) : log(x) * 0.4342944819032518; }

float powf(float x, float y)
{
    if (y == 0.0f) return 1.0f;
    if (y == 1.0f) return x;
    if (x == 0.0f) return y > 0.0f ? 0.0f : (y < 0.0f ? INFINITY : (float)NAN);
    if (x < 0.0f) return (float)NAN;   /* the core never asks */
    return expf(y * logf(x));
}
double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (y == 1.0) return x;
    if (x == 0.0) return y > 0.0 ? 0.0 : (y < 0.0 ? 1.0 / 0.0 : (double)NAN);
    if (x < 0.0) return (double)NAN;
    return exp(y * log(x));
}
/* ldexp: x * 2^e by exponent surgery, so every mantissa bit and the sign survive (stb_vorbis's float32_unpack
 * scales ±mantissas by large negative powers: the naive version below used to keep only the exponent field,
 * zeroing the mantissa and dropping the sign, which decoded every codebook float wrong and garbled the music).
 * Overflow gives signed infinity, underflow a subnormal or signed zero; zeros, infinities and NaNs pass through. */
float ldexpf(float x, int e)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    uint32_t sign = v.u & 0x80000000u;
    int exp = (int)(v.u >> 23 & 0xff);
    uint32_t mant = v.u & 0x7fffffu;
    if (exp == 0xff) return x;
    if (!exp && !mant) return x;
    uint32_t full = exp ? (mant | 0x800000u) : mant;
    long ne = (long)(exp ? exp : 1) + e;
    while (full < 0x800000u) { full <<= 1; ne--; }   /* normalize a subnormal input */
    if (ne >= 0xff) { v.u = sign | 0x7f800000u; return v.f; }
    if (ne > 0) { v.u = sign | (uint32_t)ne << 23 | (full & 0x7fffffu); return v.f; }
    long sh = 1 - ne;   /* >= 1: how far the significand sinks below the normal range */
    if (sh > 31) { v.u = sign; return v.f; }
    uint32_t out = full >> sh, rest = full & ((1u << sh) - 1u), half = 1u << (sh - 1);
    if (rest > half || (rest == half && (out & 1u))) out++;   /* round to nearest, ties to even */
    if (out > 0x7fffffu) { v.u = sign | 0x00800000u; return v.f; }   /* rounded up to the smallest normal */
    v.u = sign | out;
    return v.f;
}
double ldexp(double x, int e)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    uint64_t sign = v.u & 0x8000000000000000ull;
    int exp = (int)(v.u >> 52 & 0x7ff);
    uint64_t mant = v.u & 0xfffffffffffffull;
    if (exp == 0x7ff) return x;
    if (!exp && !mant) return x;
    uint64_t full = exp ? (mant | 0x10000000000000ull) : mant;
    long ne = (long)(exp ? exp : 1) + e;
    while (full < 0x10000000000000ull) { full <<= 1; ne--; }
    if (ne >= 0x7ff) { v.u = sign | 0x7ff0000000000000ull; return v.d; }
    if (ne > 0) { v.u = sign | (uint64_t)ne << 52 | (full & 0xfffffffffffffull); return v.d; }
    long sh = 1 - ne;
    if (sh > 63) { v.u = sign; return v.d; }
    uint64_t out = full >> sh, rest = full & ((sh >= 64 ? ~0ull : ((1ull << sh) - 1ull))), half = 1ull << (sh - 1);
    if (rest > half || (rest == half && (out & 1ull))) out++;
    if (out > 0xfffffffffffffull) { v.u = sign | 0x0010000000000000ull; return v.d; }
    v.u = sign | out;
    return v.d;
}
