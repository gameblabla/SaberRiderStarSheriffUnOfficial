/* Checks the WASM port's libm (src/platform/wasm/libm_wasm.c) against the host's libm, which is the reference.
 *
 * The core's arithmetic has to match the PC build closely enough that a run looks and plays the same (the demo's
 * own floats are reproduced on the PC and the Dreamcast), and stb_vorbis needs the double set to be right enough
 * to decode the music at all - a wrong sin, exp or floor makes it fail to find a valid codeword sequence.
 *
 * The module's functions and the host's cannot both be linked under the same names, so the module's are compiled
 * into this file with every name prefixed w_ (the -D list below) and compared against the host's.
 *
 *   cc -O2 -Isrc/platform/wasm/include -o /tmp/wasm_libm_test tools/wasm/libm_test.c src/platform/wasm/libm_wasm.c -lm
 *   /tmp/wasm_libm_test
 */
#include <stdbool.h>
#include <stdint.h>
/* The host's headers, by absolute include form: -Isrc/platform/wasm/include comes first on the path, and the
 * module's own <stdio.h> etc. would otherwise shadow them (this is a host test, not a module build). */
#include_next <stdio.h>
#include_next <stdlib.h>

/* NOTE: <math.h> is deliberately NOT included here. The include path puts the module's own maths header first,
 * so an include would pick that up; the host's libm is declared through the prefix list below instead (the
 * reference functions the checks compare against are named sin/cos/atan/... by this file's own declarations). */

/* The module's names, prefixed so the host's libm can be linked alongside. The renames come before the module's
 * own header, so its declarations land under the prefixed names too. */
#define sinf w_sinf
#define cosf w_cosf
#define tanf w_tanf
#define floorf w_floorf
#define ceilf w_ceilf
#define truncf w_truncf
#define roundf w_roundf
#define fabsf w_fabsf
#define fmodf w_fmodf
#define remainderf w_remainderf
#define copysignf w_copysignf
#define fminf w_fminf
#define fmaxf w_fmaxf
#define fdimf w_fdimf
#define sqrtf w_sqrtf
#define hypotf w_hypotf
#define asinf w_asinf
#define acosf w_acosf
#define atanf w_atanf
#define atan2f w_atan2f
#define expf w_expf
#define logf w_logf
#define log2f w_log2f
#define log10f w_log10f
#define powf w_powf
#define ldexpf w_ldexpf
#define ilogbf w_ilogbf
#define sin w_sin
#define cos w_cos
#define tan w_tan
#define asin w_asin
#define acos w_acos
#define atan w_atan
#define atan2 w_atan2
#define exp w_exp
#define log w_log
#define log2 w_log2
#define log10 w_log10
#define pow w_pow
#define floor w_floor
#define ceil w_ceil
#define trunc w_trunc
#define round w_round
#define fabs w_fabs
#define fmod w_fmod
#define remainder w_remainder
#define copysign w_copysign
#define fmin w_fmin
#define fmax w_fmax
#define fdim w_fdim
#define sqrt w_sqrt
#define hypot w_hypot
#define ldexp w_ldexp
#define ilogb w_ilogb

/* the module's own header (on the include path ahead of the host's), then its source. The renames above are
 * already in force, so both land under the w_ names. */
#include <math.h>
#include "libm_wasm.c"

/* the host's libm, for the reference side of every check. Declared here so the module's header above does not
 * shadow them: with the host's <math.h> not readable under this include path, these are the names it provides. */
double sin(double), cos(double), atan(double), atan2(double, double), exp(double), log(double), pow(double, double);
double floor(double), ceil(double), trunc(double), round(double), fabs(double), fmod(double, double);
double remainder(double, double), sqrt(double), hypot(double, double);
float sinf(float), cosf(float), atanf(float), atan2f(float, float), expf(float), logf(float), powf(float, float);
float floorf(float), ceilf(float), truncf(float), roundf(float), fabsf(float), fmodf(float, float);
float remainderf(float, float), sqrtf(float), hypotf(float, float);
double atan(double);

static int failures;

static void check(const char *what, double got, double want, double tol)
{
    if (!(fabs(got - want) <= tol)) {
        printf("  FAIL %-24s got %-18.10g want %-18.10g diff %-10.3g (tol %.3g)\n", what, got, want, got - want, tol);
        failures++;
    }
}
static void check_i(const char *what, long got, long want)
{
    if (got != want) {
        printf("  FAIL %-24s got %-18ld want %-18ld\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sin / cos, float (tol 1e-6), four quadrants, both signs, -20..20 radians\n");
    for (int i = -500; i <= 500; i++) {
        double a = i * 0.037;
        check("sinf", w_sinf((float)a), sin((float)a), 1e-6);
        check("cosf", w_cosf((float)a), cos((float)a), 1e-6);
    }
    printf("sin / cos, double (tol 1e-14)\n");
    for (int i = -500; i <= 500; i++) {
        double a = i * 0.037;
        check("sin", w_sin(a), sin(a), 1e-14);
        check("cos", w_cos(a), cos(a), 1e-14);
    }
    printf("sin / cos at the exact points\n");
    check("sin(0)", w_sinf(0.0f), 0.0, 0);
    check("sin(pi/2)", w_sinf((float)(M_PI / 2)), 1.0, 1e-6);
    check("sin(pi)", w_sinf((float)M_PI), 0.0, 1e-6);
    check("sin(3pi/2)", w_sinf((float)(3 * M_PI / 2)), -1.0, 1e-6);
    check("sin(-pi/2)", w_sinf((float)(-M_PI / 2)), -1.0, 1e-6);
    check("cos(0)", w_cosf(0.0f), 1.0, 0);
    check("cos(pi)", w_cosf((float)M_PI), -1.0, 1e-6);
    check("sin(2pi)", w_sinf((float)(2 * M_PI)), 0.0, 1e-6);
    check("cos(2pi)", w_cosf((float)(2 * M_PI)), 1.0, 1e-6);
    check("sin(1000.0)", w_sin(1000.0), sin(1000.0), 1e-12);
    check("cos(-1000.0)", w_cos(-1000.0), cos(-1000.0), 1e-12);

    printf("atan / atan2\n");
    for (int i = -200; i <= 200; i++) {
        double x = i * 0.01;
        check("atanf", w_atanf((float)x), (float)atan(x), 1e-6);
        check("atan2f", w_atan2f((float)(x * 3), (float)(5 - x)), (float)atan2(x * 3, 5 - x), 1e-6);
        check("atan", w_atan(x), atan(x), 1e-14);
    }
    check("atan2(y,0)", w_atan2f(1.0f, 0.0f), (float)(M_PI / 2), 1e-6);
    check("atan2(-y,0)", w_atan2f(-1.0f, 0.0f), (float)(-M_PI / 2), 1e-6);
    check("atan2(0,-x)", w_atan2f(0.0f, -1.0f), (float)M_PI, 1e-6);
    check("atan2(0,x)", w_atan2f(0.0f, 1.0f), 0.0, 0);

    printf("roundings (exact: r_round promises halves away from zero)\n");
    check_i("roundf(2.5)", (long)w_roundf(2.5f), 3);
    check_i("roundf(-2.5)", (long)w_roundf(-2.5f), -3);
    check_i("roundf(0.5)", (long)w_roundf(0.5f), 1);
    check_i("roundf(-0.5)", (long)w_roundf(-0.5f), -1);
    check_i("roundf(2.4)", (long)w_roundf(2.4f), 2);
    check_i("roundf(-2.4)", (long)w_roundf(-2.4f), -2);
    check_i("roundf(0)", (long)w_roundf(0.0f), 0);
    check_i("floorf(-1.5)", (long)w_floorf(-1.5f), -2);
    check_i("floorf(1.5)", (long)w_floorf(1.5f), 1);
    check_i("ceilf(-1.5)", (long)w_ceilf(-1.5f), -1);
    check_i("ceilf(1.5)", (long)w_ceilf(1.5f), 2);
    check_i("truncf(-1.5)", (long)w_truncf(-1.5f), -1);
    check_i("truncf(1.9)", (long)w_truncf(1.9f), 1);
    check_i("round(2.5)", (long)w_round(2.5), 3);
    check_i("round(-2.5)", (long)w_round(-2.5), -3);

    printf("fmod / remainder (r_fmod keeps a's sign, r_remainder rounds to nearest)\n");
    for (int i = 1; i < 20; i++) {
        double a = i * 1.37, b = 0.7 + i * 0.11;
        check("fmodf", w_fmodf((float)a, (float)b), (float)fmod(a, b), 1e-5);
        check("remainderf", w_remainderf((float)a, (float)b), (float)remainder(a, b), 1e-5);
        check("fmod", w_fmod(a, b), fmod(a, b), 1e-12);
        check("remainder", w_remainder(a, b), remainder(a, b), 1e-12);
    }
    check("fmodf(-7.5,2)", w_fmodf(-7.5f, 2.0f), -1.5f, 1e-6);
    check("fmodf(7.5,-2)", w_fmodf(7.5f, -2.0f), 1.5f, 1e-6);
    check("remainderf(7,2)", w_remainderf(7.0f, 2.0f), -1.0f, 1e-6);

    printf("sqrt / hypot\n");
    check("sqrtf(2)", w_sqrtf(2.0f), 1.4142136f, 1e-6);
    check("sqrtf(0)", w_sqrtf(0.0f), 0.0f, 0);
    check("hypotf(3,4)", w_hypotf(3.0f, 4.0f), 5.0f, 1e-6);
    check("hypotf(-3,4)", w_hypotf(-3.0f, 4.0f), 5.0f, 1e-6);
    check("hypotf(5,12)", w_hypotf(5.0f, 12.0f), 13.0f, 1e-6);

    printf("exp / log / pow\n");
    check("expf(0)", w_expf(0.0f), 1.0f, 0);
    check("expf(1)", w_expf(1.0f), 2.7182817f, 1e-6);
    check("expf(-1)", w_expf(-1.0f), 0.36787945f, 1e-7);
    check("exp(0)", w_exp(0.0), 1.0, 0);
    check("exp(1)", w_exp(1.0), 2.718281828459045, 1e-14);
    check("exp(-1)", w_exp(-1.0), 0.36787944117144233, 1e-15);
    check("exp(10)", w_exp(10.0), 22026.465794806718, 1e-9);
    check("logf(1)", w_logf(1.0f), 0.0f, 0);
    check("log(1)", w_log(1.0), 0.0, 0);
    check("log(100)", w_log(100.0), 4.6051701859880914, 1e-13);
    check("log(0.5)", w_log(0.5), -0.69314718055994531, 1e-14);
    for (int i = 1; i < 10; i++) {
        double x = i * 0.37;
        check("log", w_log(x), log(x), 1e-14);
        check("exp", w_exp(x), exp(x), 1e-14);
        check("pow", w_pow(x, 2.5), pow(x, 2.5), 1e-12);
    }
    check("powf(2,10)", w_powf(2.0f, 10.0f), 1024.0f, 1e-3);
    check("pow(2,0.5)", w_pow(2.0, 0.5), 1.4142135623730951, 1e-12);

    printf("ilogb / ldexp (exact)\n");
    check_i("ilogb(1)", w_ilogb(1.0), 0);
    check_i("ilogb(1024)", w_ilogb(1024.0), 10);
    check_i("ilogb(0.5)", w_ilogb(0.5), -1);
    check_i("ilogb(1e-30)", w_ilogb(1e-30), -100);
    check_i("ilogbf(1024)", w_ilogbf(1024.0f), 10);
    check_i("ilogbf(0.25)", w_ilogbf(0.25f), -2);
    check("ldexp(1,10)", w_ldexp(1.0, 10), 1024.0, 0);
    check("ldexpf(1,-2)", w_ldexpf(1.0f, -2), 0.25, 0);
    /* ldexp has to keep every mantissa bit and the sign (stb_vorbis's float32_unpack scales ±mantissas by
     * large negative powers; a version keeping only the exponent field decodes every codebook float wrong).
     * The two checks above only cover powers of two, which pass either way. */
    check("ldexp(1.5,1)", w_ldexp(1.5, 1), 3.0, 0);
    check("ldexp(-2.0,3)", w_ldexp(-2.0, 3), -16.0, 0);
    check("ldexp(1.5,-1)", w_ldexp(1.5, -1), 0.75, 0);
    check("ldexpf(1234567,-2)", w_ldexpf(1234567.0f, -2), 308641.75f, 0);
    check("ldexpf(-0.75,2)", w_ldexpf(-0.75f, 2), -3.0f, 0);
    check("ldexp(2097151,-20)", w_ldexp(2097151.0, -20), 2097151.0 / 1048576.0, 0);
    check("ldexp(1,-1074)", w_ldexp(1.0, -1074), 5e-324, 0);   /* smallest subnormal */
    check("ldexp(1,-1075)", w_ldexp(1.0, -1075), 0.0, 0);      /* underflow to zero */
    if (w_ldexp(1.0, 1024) != 1.0 / 0.0) { printf("FAIL ldexp(1,1024) overflow\n"); failures++; }
    check("ldexpf(1,-149)", w_ldexpf(1.0f, -149), 1.4012984643e-45f, 0);
    check("ldexpf(1,-150)", w_ldexpf(1.0f, -150), 0.0f, 0);
    if (w_ldexpf(1.0f, 128) != 1.0f / 0.0f) { printf("FAIL ldexpf(1,128) overflow\n"); failures++; }
    if (w_ldexp(-0.0, 5) != 0.0 || 1 / w_ldexp(-0.0, 5) > 0) { printf("FAIL ldexp(-0,5) sign\n"); failures++; }
    if (w_ldexpf(-0.0f, 5) != 0.0f || 1 / w_ldexpf(-0.0f, 5) > 0) { printf("FAIL ldexpf(-0,5) sign\n"); failures++; }

    if (failures) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("\nall libm checks passed\n");
    return 0;
}
