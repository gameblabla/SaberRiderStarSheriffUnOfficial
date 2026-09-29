/* Checks the WASM port's printf float conversions (src/platform/wasm/libc_wasm.c) against the host's glibc.
 *
 * The core prints reals through real.h's RS() / RSG() ("%.*f" and "%g"), which the debug and SABER_PERF output
 * use for every coordinate it reports - so a wrong digit or a crash here shows up as unreadable or missing
 * console output rather than as wrong pixels. It is a host test: libc_wasm.c is included directly (with the two
 * imports and the two wasm builtins stubbed) and the results are written with write(2), because the module's own
 * stdout is a console sink (vfs_wasm.c).
 *
 *   cc -O1 -Isrc/platform/wasm/include -Isrc/platform/wasm -o /tmp/wasm_fmt_test tools/wasm/fmt_test.c -lm
 *   /tmp/wasm_fmt_test            # diff its output against the same list printed by glibc
 *
 * Two differences from glibc are expected and fine: a value past 2^64 printed with %f (the digit extraction runs
 * through repeated division and so drifts in the last places, where glibc is exact), and an exact decimal tie in
 * %e (the binary mantissa is never exactly the decimal tie, so the rounding can go the other way). The game's
 * reals are screen coordinates and angles, far from both. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

double js_now_ms(void) { return 0; }
void js_log(int e, const char *t, uint32_t n) { (void)e; (void)t; (void)n; }
unsigned __builtin_wasm_memory_size(unsigned i) { (void)i; return 16; }
unsigned __builtin_wasm_memory_grow(unsigned i, unsigned p) { (void)i; (void)p; return (unsigned)-1; }
unsigned char __heap_base;

#include "libc_wasm.c"

extern int write(int fd, const void *buf, unsigned long n);

static void say(const char *s) { write(1, s, strlen(s)); }
static void say_int(int v)
{
    char d[24];
    int k = 0, a = v < 0 ? -v : v;
    do { d[k++] = (char)('0' + a % 10); a /= 10; } while (a);
    char o[24];
    int m = 0;
    if (v < 0) o[m++] = '-';
    for (int i = k - 1; i >= 0; i--) o[m++] = d[i];
    o[m] = 0;
    say(o);
}

int main(void)
{
    static const char *const CASES[] = { "%f", "%.0f", "%.1f", "%.3f", "%e", "%.3e", "%g", "%.2g", "%.10g", "%G", "%.0e" };
    static const double VALS[] = { 0.0, -0.0, 1.0, -2.5, 3.14159, 1234.5, 0.0001234, 123456789.0, 1e-30, 1e30, 0.5, -0.75 };
    char b[512];
    say("# format value -> text\n");
    for (unsigned i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        for (unsigned j = 0; j < sizeof VALS / sizeof *VALS; j++) {
            int n = snprintf(b, sizeof b, CASES[i], VALS[j]);
            say(CASES[i]); say(" -> "); say(b); say("  (n="); say_int(n); say(")\n");
        }
    }
    /* the mixed forms the core actually prints: a label, several reals and two hex numbers on one line */
    snprintf(b, sizeof b, "cam=%d pos=%.1f,%.1f v=%.0f,%.0f flags=%x coll=%x", 0, 6.75, 177.0, 100.0, -0.5, 0x1f, 0x4);
    say("mixed -> "); say(b); say("\n");
    return 0;
}
