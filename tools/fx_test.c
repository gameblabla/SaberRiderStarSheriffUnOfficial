/* Checks src/fx.c against libm / exact integer references (make test).
 *   cc -O2 -Isrc tools/fx_test.c src/fx.c -lm -o /tmp/fx_test && /tmp/fx_test */
#include "fx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { if (fails++ < 20) { printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main(void)
{
    /* conversions and rounding */
    CHECK(FX(1.5) == 0x18000 && FX(-0.25) == -0x4000 && FX_DEG(90) == 16384, "FX constants");
    CHECK(fx_floor(FX(-0.5)) == -1 && fx_ceil(FX(-0.5)) == 0 && fx_round(FX(2.5)) == 3 && fx_round(FX(-2.5)) == -2, "rounding");
    CHECK(fx_from_float(3.25f) == FX(3.25) && fx_to_float(FX(-7.75)) == -7.75f, "float conversions");
    for (int i = 0; i < 1000000; i++) {
        int32_t a = (int32_t)rnd() >> (rnd() % 16), b = (int32_t)rnd() >> (rnd() % 24);
        int64_t m = ((int64_t)a * b) >> 16;
        if (m >= INT32_MIN && m <= INT32_MAX) CHECK(fx_mul(a, b) == (fx)m, "mul %d %d", a, b);
        if (b) {
            int64_t q = ((int64_t)a * 65536) / b;
            fx want = q > INT32_MAX ? FX_MAX : q < INT32_MIN ? FX_MIN : (fx)q;
            CHECK(fx_div(a, b) == want, "div %d %d: %d want %d", a, b, fx_div(a, b), want);
            int32_t c = (int32_t)rnd() >> (rnd() % 20);
            if (c) {
                int64_t r = ((int64_t)a * b) / c;
                fx w2 = r > INT32_MAX ? FX_MAX : r < INT32_MIN ? FX_MIN : (fx)r;
                CHECK(fx_muldiv(a, b, c) == w2, "muldiv %d %d %d", a, b, c);
            }
        }
        uint32_t s = rnd() >> 1;
        uint64_t r = (uint64_t)sqrt((double)((uint64_t)s << 16));
        while (r * r > ((uint64_t)s << 16)) r--;
        while ((r + 1) * (r + 1) <= ((uint64_t)s << 16)) r++;
        CHECK((uint64_t)fx_sqrt((fx)s) == r, "sqrt %u", s);
        int32_t x = (int32_t)rnd() >> (rnd() % 20), y = (int32_t)rnd() >> (rnd() % 20);
        double h = sqrt((double)x * x + (double)y * y);
        CHECK(fabs(fx_hypot(x, y) - h) <= 1.0 || h > 2147483647.0, "hypot %d %d: %d want %.1f", x, y, fx_hypot(x, y), h);
    }
    /* trigonometry */
    double worst_s = 0, worst_a = 0;
    for (int a = 0; a < 65536; a++) {
        double ref = sin(a * 2 * M_PI / 65536) * 65536;
        double e = fabs(fx_sin(a) - ref) / 65536;
        if (e > worst_s) worst_s = e;
        CHECK(fx_cos(a) == fx_sin(a + 16384), "cos");
    }
    CHECK(fx_sin(0) == 0 && fx_sin(16384) == 65536 && fx_sin(32768) == 0 && fx_sin(-16384) == -65536, "sin exact points");
    for (int i = 0; i < 1000000; i++) {
        int32_t x = (int32_t)rnd() >> (rnd() % 31), y = (int32_t)rnd() >> (rnd() % 31);
        if (!x && !y) continue;
        double ref = atan2((double)y, (double)x) * 65536 / (2 * M_PI);
        double e = fabs((double)fx_atan2(y, x) - ref);
        if (e > 32768) e = 65536 - e;
        if (e > worst_a) worst_a = e;
    }
    CHECK(fx_atan2(0, FX(1)) == 0 && fx_atan2(FX(1), 0) == 16384 && fx_atan2(0, FX(-1)) == -32768 && fx_atan2(-FX(1), 0) == -16384, "atan2 axes");
    CHECK(worst_s < 1.6e-5, "sin error %g", worst_s);
    CHECK(worst_a < 2.0, "atan2 error %g binary-angle units", worst_a);   /* 2/65536 of a turn = 0.011 degrees */
    CHECK(fx_ang_from_rad(FX(3.14159265)) == 32768 && abs(fx_rad_from_ang(16384) - FX(1.5707963)) <= 1, "rad conversions");
    printf("fx: sin error %.2g, atan2 error %.2f / 65536 turn; %s\n", worst_s, worst_a, fails ? "FAILED" : "ok");
    return fails != 0;
}
