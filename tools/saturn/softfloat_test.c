/* Host check of src/platform/saturn/softfloat_sat.c against the FPU: every operation on random and edge-case operands
 * must give the same bits (results or operands that are denormal are skipped: the Saturn flushes them to zero).
 *   cc -O2 -DSF_HOST_TEST tools/saturn/softfloat_test.c -o /tmp/sft && /tmp/sft */
#include "../../src/platform/saturn/softfloat_sat.c"
#include <stdio.h>
#include <string.h>
#include <math.h>

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static int denorm(uint32_t u) { return (u & 0x7F800000u) == 0 && (u & 0x7FFFFFu); }
static int isnanb(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

static uint32_t operand(void)
{
    switch (rnd() % 8) {
    case 0: { static const uint32_t e[] = { 0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7F7FFFFFu,
                                            0x00800000u, 0x80800000u, 0x3F7FFFFFu, 0x4B000000u, 0x4AFFFFFFu, 0x3F000000u };
              return e[rnd() % (sizeof e / sizeof *e)]; }
    case 1: return bits((float)(int32_t)(rnd() % 2001) - 1000.0f);                   /* small integers */
    case 2: return bits((float)((int32_t)rnd() % 100000) / 64.0f);                   /* game-like coordinates */
    case 3: return (rnd() & 0x80000000u) | ((120 + rnd() % 16) << 23) | (rnd() & 0x7FFFFFu);   /* near 1 */
    default: return rnd();
    }
}

int main(void)
{
    long bad = 0, n = 0;
    for (long i = 0; i < 20000000; i++) {
        uint32_t a = operand(), b = operand();
        if (denorm(a) || denorm(b)) continue;
        float fa = flt(a), fb = flt(b);
        struct { const char *op; uint32_t want, got; } t[] = {
            { "add", bits(fa + fb), sf___addsf3(a, b) }, { "sub", bits(fa - fb), sf___subsf3(a, b) },
            { "mul", bits(fa * fb), sf___mulsf3(a, b) }, { "div", bits(fa / fb), sf___divsf3(a, b) } };
        for (int k = 0; k < 4; k++) {
            if (denorm(t[k].want) || (t[k].want & 0x7FFFFFFFu) < 0x00800000u) {   /* underflow: we give a zero */
                if ((t[k].got & 0x7FFFFFFFu) != 0) { if (bad++ < 20) printf("%s %08x %08x: want ~0, got %08x\n", t[k].op, a, b, t[k].got); }
                continue;
            }
            n++;
            if ((t[k].want & 0x7FFFFFFFu) == 0x00800000u && (t[k].got & 0x7FFFFFFFu) == 0) continue;   /* rounded up out of the denormals */
            if (isnanb(t[k].want) ? !isnanb(t[k].got) : t[k].want != t[k].got)
                if (bad++ < 20) printf("%s %08x %08x (%g %g): want %08x got %08x\n", t[k].op, a, b, fa, fb, t[k].want, t[k].got);
        }
        /* comparisons */
        int lt = fa < fb, le = fa <= fb, gt = fa > fb, ge = fa >= fb, eq = fa == fb, un = isnan(fa) || isnan(fb);
        if ((sf___ltsf2(a, b) < 0) != lt || (sf___lesf2(a, b) <= 0) != le || (sf___gtsf2(a, b) > 0) != gt ||
            (sf___gesf2(a, b) >= 0) != ge || (sf___eqsf2(a, b) == 0) != eq || (sf___nesf2(a, b) != 0) != !eq ||
            (sf___unordsf2(a, b) != 0) != un)
            if (bad++ < 20) printf("cmp %08x %08x (%g %g)\n", a, b, fa, fb);
        /* conversions */
        int32_t iv = (int32_t)rnd() >> (rnd() % 31);
        if (sf___floatsisf(iv) != bits((float)iv)) if (bad++ < 20) printf("floatsi %d: want %08x got %08x\n", iv, bits((float)iv), sf___floatsisf(iv));
        uint32_t uv = rnd() >> (rnd() % 31);
        if (sf___floatunsisf(uv) != bits((float)uv)) if (bad++ < 20) printf("floatunsi %u\n", uv);
        if (!isnanb(a) && fabsf(fa) < 2.0e9f && sf___fixsfsi(a) != (int32_t)fa) if (bad++ < 20) printf("fixsf %g: want %d got %d\n", fa, (int32_t)fa, sf___fixsfsi(a));
        if (!isnanb(a) && fa >= 0 && fa < 4.0e9f && sf___fixunssfsi(a) != (uint32_t)fa) if (bad++ < 20) printf("fixunssf %g\n", fa);
    }
    printf("%ld results checked, %ld wrong\n", n, bad);
    return bad != 0;
}
