/* The C library pieces libyaul lacks or has in a reduced form, for the core game on the Saturn:
 *   - the heaps: TLSF pools over high work RAM (after the program) and low work RAM (the whole 1 MB). malloc puts big
 *     blocks in low RAM (pack blocks, level data: read by the CPUs only), small ones in high RAM; hw_malloc is always
 *     high RAM, the only work RAM the SCU DMA can read (plan 8.5);
 *   - the float maths (soft-float: the SH-2 has no FPU; the core's hot paths use fx.h instead);
 *   - printf / snprintf with %f, %u, %l... (libyaul's reads %f as a 16.16 integer), sscanf / fscanf, fgets, fmemopen,
 *     qsort, strtod, calloc, aligned_alloc.
 * Files on the CD are opened by cd_sat.c's fopen, stdout / stderr go to the log ring (log_sat.c). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <mm/tlsf.h>

/* ---------------------------------------------------------------- heaps */
#define HWRAM_END  0x06100000u
#define LWRAM_BASE 0x00200000u
#define LWRAM_END  0x00300000u
#define BIG_BLOCK  4096          /* malloc: blocks this big go to low RAM first */

extern uint8_t __end[];          /* the linker script's ___end: end of .bss / .uncached */
static tlsf_t hw_heap, lw_heap;
static size_t hw_used, lw_used;

static void heaps_init(void)
{
    if (hw_heap) return;
    uintptr_t start = ((uintptr_t)__end + 63) & ~(uintptr_t)63;
    hw_heap = tlsf_pool_create((void *)start, HWRAM_END - start);
    lw_heap = tlsf_pool_create((void *)LWRAM_BASE, LWRAM_END - LWRAM_BASE);
}

static bool in_lw(const void *p) { return (uintptr_t)p >= LWRAM_BASE && (uintptr_t)p < LWRAM_END; }

static void *pool_alloc(tlsf_t h, size_t align, size_t n)
{
    void *p = align > 4 ? tlsf_memalign(h, align, n) : tlsf_malloc(h, n);
    if (p) { if (h == lw_heap) lw_used += tlsf_block_size(p); else hw_used += tlsf_block_size(p); }
    return p;
}

static void *any_alloc(size_t align, size_t n)
{
    heaps_init();
    if (n == 0) n = 1;
    void *p;
    if (n >= BIG_BLOCK) { if (!(p = pool_alloc(lw_heap, align, n))) p = pool_alloc(hw_heap, align, n); }
    else if (!(p = pool_alloc(hw_heap, align, n))) p = pool_alloc(lw_heap, align, n);
    return p;
}

void *malloc(size_t n) { return any_alloc(4, n); }
void *memalign(size_t align, size_t n) { return any_alloc(align, n); }
void *aligned_alloc(size_t align, size_t n) { return any_alloc(align, n); }
void *hw_malloc(size_t n) { heaps_init(); return pool_alloc(hw_heap, 4, n ? n : 1); }
void *hw_memalign(size_t align, size_t n) { heaps_init(); return pool_alloc(hw_heap, align, n ? n : 1); }

void free(void *p)
{
    if (!p) return;
    if (in_lw(p)) { lw_used -= tlsf_block_size(p); tlsf_free(lw_heap, p); }
    else { hw_used -= tlsf_block_size(p); tlsf_free(hw_heap, p); }
}

void *calloc(size_t n, size_t size)
{
    size_t t = n * size;
    if (size && t / size != n) return NULL;
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void *realloc(void *old, size_t n)
{
    if (!old) return malloc(n);
    if (!n) { free(old); return NULL; }
    size_t have = tlsf_block_size(old);
    if (have >= n) return old;
    void *p = malloc(n);
    if (!p) return NULL;
    memcpy(p, old, have);
    free(old);
    return p;
}

void sat_heap_stats(size_t *hw_free, size_t *lw_free, size_t *hwu, size_t *lwu)
{
    heaps_init();
    uintptr_t start = ((uintptr_t)__end + 63) & ~(uintptr_t)63;
    if (hw_free) *hw_free = HWRAM_END - start - hw_used;
    if (lw_free) *lw_free = LWRAM_END - LWRAM_BASE - lw_used;
    if (hwu) *hwu = hw_used;
    if (lwu) *lwu = lw_used;
}

/* ---------------------------------------------------------------- stdlib */
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
char *getenv(const char *name) { (void)name; return NULL; }   /* debug switches: plat_getenv (SABER.ENV) */

/* rand: glibc's generator (its TYPE_3 additive feedback, r[i] = r[i-3] + r[i-31]), so a run draws the same numbers as
 * the PC build (libyaul's is a bare xorshift that returns negatives and never moves from a zero seed) */
static int32_t rnd_r[34]; static int rnd_i = -1;
void srand(unsigned seed)
{
    int32_t *r = rnd_r;
    r[0] = (int32_t)(seed ? seed : 1);
    for (int i = 1; i < 31; i++) {
        int32_t hi = r[i - 1] / 127773, lo = r[i - 1] % 127773, w = 16807 * lo - 2836 * hi;
        r[i] = w < 0 ? w + 2147483647 : w;
    }
    for (int i = 31; i < 34; i++) r[i] = r[i - 31];
    rnd_i = 34;
    for (int k = 0; k < 310; k++) (void)rand();
}
int rand(void)
{
    if (rnd_i < 0) srand(1);
    /* a ring of the last 34 values: r[n] = r[n-31] + r[n-3] */
    int32_t v = (int32_t)((uint32_t)rnd_r[(rnd_i - 31) % 34] + (uint32_t)rnd_r[(rnd_i - 3) % 34]);
    rnd_r[rnd_i % 34] = v;
    rnd_i = (rnd_i + 1) % 34 + 34;
    return (int)((uint32_t)v >> 1);
}

static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }

/* the core sorts a few dozen draw items at most: an insertion sort below 16, a median-of-three quicksort above */
static void qsort_r_(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    while (n > 16) {
        char *lo = base, *mid = base + (n / 2) * sz, *hi = base + (n - 1) * sz;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, sz);
        if (cmp(hi, mid) < 0) { swap_bytes(hi, mid, sz); if (cmp(mid, lo) < 0) swap_bytes(mid, lo, sz); }
        swap_bytes(mid, hi - sz, sz);   /* pivot next to the end */
        char *piv = hi - sz, *i = lo, *j = piv;
        for (;;) {
            do i += sz; while (cmp(i, piv) < 0);
            do j -= sz; while (j > lo && cmp(piv, j) < 0);
            if (i >= j) break;
            swap_bytes(i, j, sz);
        }
        swap_bytes(i, piv, sz);
        size_t left = (size_t)(i - base) / sz, right = n - left - 1;
        if (left < right) { qsort_r_(base, left, sz, cmp); base = i + sz; n = right; }
        else { qsort_r_(i + sz, right, sz, cmp); n = left; }
    }
    for (size_t k = 1; k < n; k++)
        for (char *p = base + k * sz; p > base && cmp(p - sz, p) > 0; p -= sz) swap_bytes(p - sz, p, sz);
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) { if (n > 1 && size) qsort_r_(base, n, size, cmp); }

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0; const char *start = p;
    for (;; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? (tolower((unsigned char)*p) - 'a' + 10) : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}

float strtof(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    double v = 0; bool any = false;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); any = true; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (isdigit((unsigned char)*p)) { v += (*p++ - '0') * f; f *= 0.1; any = true; }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1; bool eneg = false; int e = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) e = e * 10 + (*q++ - '0');
            while (e-- > 0) v = eneg ? v * 0.1 : v * 10;
            p = q;
        }
    }
    if (end) *end = (char *)(any ? p : s);
    return (float)(neg ? -v : v);
}
double strtod(const char *s, char **end) { return strtof(s, end); }
double atof(const char *s) { return strtof(s, NULL); }

/* ---------------------------------------------------------------- maths */
typedef union { float f; uint32_t u; int32_t i; } FU;

float fabsf(float x) { FU v = { x }; v.u &= 0x7FFFFFFFu; return v.f; }
float copysignf(float x, float y) { FU a = { x }, b = { y }; a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u); return a.f; }
float fminf(float a, float b) { return a < b ? a : b; }
float fmaxf(float a, float b) { return a > b ? a : b; }

float truncf(float x)
{
    FU v = { x };
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    if (e >= 23) return x;
    if (e < 0) { v.u &= 0x80000000u; return v.f; }
    v.u &= ~(0x007FFFFFu >> e);
    return v.f;
}
float floorf(float x) { float t = truncf(x); return t > x ? t - 1.0f : t; }
float ceilf(float x) { float t = truncf(x); return t < x ? t + 1.0f : t; }
float roundf(float x) { float t = truncf(x), d = x - t; return d >= 0.5f ? t + 1.0f : d <= -0.5f ? t - 1.0f : t; }
long lroundf(float x) { return (long)roundf(x); }
float fmodf(float x, float y) { return y == 0.0f ? 0.0f : x - truncf(x / y) * y; }
float remainderf(float x, float y)
{
    if (y == 0.0f) return 0.0f;
    float q = x / y, n = roundf(q);
    if (fabsf(q - truncf(q)) == 0.5f) n = 2.0f * roundf(q * 0.5f);   /* halfway: to even */
    return x - n * y;
}

float sqrtf(float x)
{
    FU v = { x };
    if (v.i <= 0 || (v.u >> 23) == 0xFF) return v.i < 0 ? 0.0f : x;
    v.u = (v.u >> 1) + 0x1FBD1DF5u;   /* within ~4 %, then three Newton steps */
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return 0.5f * (y + x / y);
}
float hypotf(float x, float y)
{
    x = fabsf(x); y = fabsf(y);
    float a = x > y ? x : y, b = x > y ? y : x;
    if (a == 0.0f) return 0.0f;
    float r = b / a;
    return a * sqrtf(1.0f + r * r);
}

/* sin / cos: x reduced by k * pi/2 (two-part pi/2), polynomials on [-pi/4, pi/4] (fdlibm's kernel coefficients) */
static float ksin(float r) { float z = r * r; return r + r * z * (-1.6666654611e-01f + z * (8.3321608736e-03f + z * -1.9515295891e-04f)); }
static float kcos(float r) { float z = r * r; return 1.0f - 0.5f * z + z * z * (4.1666645683e-02f + z * (-1.3887316255e-03f + z * 2.4433157118e-05f)); }
static float reduce(float x, int *q)
{
    float k = roundf(x * 0.63661977236758134f);
    *q = (int)(long)k;
    return (x - k * 1.5707963705062866f) + k * 4.3711388286737929e-08f;
}
float sinf(float x)
{
    int q; float r = reduce(x, &q);
    switch (q & 3) { case 0: return ksin(r); case 1: return kcos(r); case 2: return -ksin(r); default: return -kcos(r); }
}
float cosf(float x)
{
    int q; float r = reduce(x, &q);
    switch (q & 3) { case 0: return kcos(r); case 1: return -ksin(r); case 2: return -kcos(r); default: return ksin(r); }
}
float tanf(float x) { float c = cosf(x); return c == 0.0f ? 0.0f : sinf(x) / c; }

/* atan: cephes atanf (range reduction by tan(pi/8), tan(3pi/8)) */
float atanf(float x)
{
    float a = fabsf(x), y0 = 0.0f;
    if (a > 2.414213562373095f) { y0 = 1.5707963267948966f; a = -1.0f / a; }
    else if (a > 0.4142135623730950f) { y0 = 0.7853981633974483f; a = (a - 1.0f) / (a + 1.0f); }
    float z = a * a;
    float y = y0 + ((((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * a + a);
    return x < 0 ? -y : y;
}
float atan2f(float y, float x)
{
    if (x == 0.0f) return y > 0 ? 1.5707963267948966f : y < 0 ? -1.5707963267948966f : 0.0f;
    float t = atanf(y / x);
    if (x > 0) return t;
    return y >= 0 ? t + 3.14159265358979f : t - 3.14159265358979f;
}

/* ---------------------------------------------------------------- printf */
typedef struct { char *buf; size_t cap, n; FILE *f; } Out;

static void out_ch(Out *o, char c)
{
    if (o->f) { o->f->write(o->f, (const unsigned char *)&c, 1); o->n++; return; }
    if (o->n + 1 < o->cap) o->buf[o->n] = c;
    o->n++;
}
static void out_str(Out *o, const char *s, size_t n)
{
    if (o->f) { if (n) o->f->write(o->f, (const unsigned char *)s, n); o->n += n; return; }
    while (n--) out_ch(o, *s++);
}
static void out_pad(Out *o, char c, int n) { while (n-- > 0) out_ch(o, c); }

/* the digits of a double with prec decimals into buf (no sign), fixed notation */
static int fmt_fixed(char *buf, int cap, double v, int prec)
{
    if (prec > 9) prec = 9;
    double scale = 1; for (int i = 0; i < prec; i++) scale *= 10;
    double r = v * scale + 0.5;
    if (r > 1.8e19) { int n = 0; buf[n++] = 'i'; buf[n++] = 'n'; buf[n++] = 'f'; return n; }
    unsigned long long whole = (unsigned long long)r;
    unsigned long long ip = whole, fp = 0;
    if (prec) { unsigned long long sc = (unsigned long long)scale; ip = whole / sc; fp = whole % sc; }
    char tmp[32]; int n = 0;
    do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < 30);
    int k = 0;
    while (n && k < cap) buf[k++] = tmp[--n];
    if (prec && k < cap) {
        buf[k++] = '.';
        char d[12]; for (int i = prec - 1; i >= 0; i--) { d[i] = (char)('0' + fp % 10); fp /= 10; }
        for (int i = 0; i < prec && k < cap; i++) buf[k++] = d[i];
    }
    return k;
}

static int vformat(Out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_ch(o, *fmt); continue; }
        fmt++;
        bool left = false, plus = false, space = false, zero = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true; else if (*fmt == '+') plus = true; else if (*fmt == ' ') space = true;
            else if (*fmt == '0') zero = true; else if (*fmt == '#') alt = true; else break;
        }
        int width = -1, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; }
        else if (isdigit((unsigned char)*fmt)) { width = 0; while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0'); }
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;   /* 1 long, 2 long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lng = lng == 1 ? 2 : 1;
            else if (*fmt == 'h') lng = lng == -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lng = *fmt == 'j' ? 2 : 1;
            else break;
        }
        char c = *fmt, buf[48]; int n = 0; const char *s = buf; char sign = 0;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            sign = v < 0 ? '-' : plus ? '+' : space ? ' ' : 0;
            char t[24]; int k = 0; do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            while (k < prec) t[k++] = '0';
            while (k) buf[n++] = t[--k];
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long u = c == 'p' ? (uintptr_t)va_arg(ap, void *)
                                 : lng == 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (lng == -1) u = (unsigned short)u; else if (lng == -2) u = (unsigned char)u;
            unsigned base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char t[24]; int k = 0; do { t[k++] = dig[u % base]; u /= base; } while (u);
            while (k < prec) t[k++] = '0';
            if ((alt && base == 16) || c == 'p') { buf[n++] = '0'; buf[n++] = c == 'X' ? 'X' : 'x'; }
            while (k) buf[n++] = t[--k];
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            if (v < 0) { sign = '-'; v = -v; } else sign = plus ? '+' : space ? ' ' : 0;
            if (prec < 0) prec = 6;
            if (c == 'g' || c == 'G') {   /* close enough for logs: fixed with trailing zeros dropped */
                n = fmt_fixed(buf, sizeof buf, v, prec > 6 ? 6 : prec);
                if (memchr(buf, '.', n)) { while (n && buf[n - 1] == '0') n--; if (n && buf[n - 1] == '.') n--; }
            } else if (c == 'e' || c == 'E') {
                int e = 0;
                if (v != 0) { while (v >= 10) { v /= 10; e++; } while (v < 1) { v *= 10; e--; } }
                n = fmt_fixed(buf, sizeof buf - 6, v, prec);
                buf[n++] = c; buf[n++] = e < 0 ? '-' : '+'; if (e < 0) e = -e;
                if (e >= 100) buf[n++] = (char)('0' + e / 100);
                buf[n++] = (char)('0' + e / 10 % 10); buf[n++] = (char)('0' + e % 10);
            } else n = fmt_fixed(buf, sizeof buf, v, prec);
            break;
        }
        case 'c': buf[n++] = (char)va_arg(ap, int); break;
        case 's': {
            s = va_arg(ap, const char *); if (!s) s = "(null)";
            n = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            break;
        }
        case 'n': { int *p = va_arg(ap, int *); if (p) *p = (int)o->n; continue; }
        case '%': out_ch(o, '%'); continue;
        default: out_ch(o, '%'); out_ch(o, c); continue;
        }
        int len = n + (sign ? 1 : 0);
        bool zpad = zero && !left && c != 's' && c != 'c' && !(prec >= 0 && (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X'));
        if (!left && !zpad) out_pad(o, ' ', width - len);
        if (sign) out_ch(o, sign);
        if (zpad) out_pad(o, '0', width - len);
        out_str(o, s, (size_t)n);
        if (left) out_pad(o, ' ', width - len);
    }
    return (int)o->n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    Out o = { buf, cap, 0, NULL };
    int r = vformat(&o, fmt, ap);
    if (cap) buf[o.n < cap ? o.n : cap - 1] = 0;
    return r;
}
int snprintf(char *buf, size_t cap, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r; }
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)INT_MAX, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { Out o = { NULL, 0, 0, f }; return vformat(&o, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
int puts(const char *s) { size_t n = strlen(s); stdout->write(stdout, (const unsigned char *)s, n); stdout->write(stdout, (const unsigned char *)"\n", 1); return 0; }
int fputs(const char *s, FILE *f) { size_t n = strlen(s); return f->write(f, (const unsigned char *)s, n) == n ? 0 : EOF; }
int fputc(int c, FILE *f) { unsigned char b = (unsigned char)c; return f->write(f, &b, 1) == 1 ? c : EOF; }
int putchar(int c) { return fputc(c, stdout); }

/* ---------------------------------------------------------------- scanf */
typedef struct { const char *s; FILE *f; int la; int count; } In;   /* la: one character looked ahead (-2 none) */

static int in_peek(In *in)
{
    if (in->s) return *in->s ? (unsigned char)*in->s : EOF;
    if (in->la == -2) in->la = getc(in->f);
    return in->la;
}
static void in_next(In *in) { if (in->s) { if (*in->s) in->s++; } else in->la = -2; in->count++; }
static void in_done(In *in) { if (in->f && in->la >= 0) fseek(in->f, -1, SEEK_CUR); }   /* give back the look-ahead */

static int vscan(In *in, const char *fmt, va_list ap)
{
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { while (isspace(in_peek(in))) in_next(in); continue; }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (in_peek(in) != (unsigned char)*fmt) break;
            in_next(in); continue;
        }
        fmt++;
        bool skip = false; if (*fmt == '*') { skip = true; fmt++; }
        int width = 0; while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int lng = 0; while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { lng = *fmt == 'h' ? -1 : lng + 1; fmt++; }
        char c = *fmt;
        if (c == 'n') { if (!skip) *va_arg(ap, int *) = in->count; continue; }
        if (c != 'c' && c != '[') while (isspace(in_peek(in))) in_next(in);
        if (in_peek(in) == EOF) return assigned ? assigned : EOF;
        if (width <= 0) width = INT_MAX;
        char tok[64]; int n = 0;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            int base = c == 'x' || c == 'X' ? 16 : 10;
            if ((in_peek(in) == '-' || in_peek(in) == '+') && n < width) { tok[n++] = (char)in_peek(in); in_next(in); }
            while (n < width && n < 62) {
                int ch = in_peek(in);
                if (!(base == 16 ? isxdigit(ch) : isdigit(ch))) break;
                tok[n++] = (char)ch; in_next(in);
            }
            tok[n] = 0;
            if (!n || (n == 1 && (tok[0] == '-' || tok[0] == '+'))) break;
            if (!skip) {
                long v = c == 'u' || base == 16 ? (long)strtoul(tok, NULL, base) : strtol(tok, NULL, base);
                if (lng > 0) *va_arg(ap, long *) = v; else if (lng < 0) *va_arg(ap, short *) = (short)v; else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
        } else if (c == 'f' || c == 'g' || c == 'e' || c == 'E' || c == 'G') {
            bool dot = false, exp = false;
            while (n < width && n < 62) {
                int ch = in_peek(in);
                if (isdigit(ch)) ;
                else if ((ch == '-' || ch == '+') && (n == 0 || tok[n - 1] == 'e' || tok[n - 1] == 'E')) ;
                else if (ch == '.' && !dot && !exp) dot = true;
                else if ((ch == 'e' || ch == 'E') && !exp && n) exp = true;
                else break;
                tok[n++] = (char)ch; in_next(in);
            }
            tok[n] = 0;
            if (!n) break;
            if (!skip) { float v = strtof(tok, NULL); if (lng > 0) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = v; assigned++; }
        } else if (c == 's') {
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width) { int ch = in_peek(in); if (ch == EOF || isspace(ch)) break; if (d) *d++ = (char)ch; n++; in_next(in); }
            if (d) { *d = 0; assigned++; }
        } else if (c == 'c') {
            char *d = skip ? NULL : va_arg(ap, char *);
            if (width == INT_MAX) width = 1;
            while (n < width) { int ch = in_peek(in); if (ch == EOF) break; if (d) *d++ = (char)ch; n++; in_next(in); }
            if (!n) break;
            if (d) assigned++;
        } else if (c == '[') {
            fmt++;
            bool neg = false; if (*fmt == '^') { neg = true; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            size_t setn = (size_t)(fmt - set);
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width) {
                int ch = in_peek(in); if (ch == EOF) break;
                bool in_set = false;
                for (size_t k = 0; k < setn; k++) {
                    if (k + 2 < setn && set[k + 1] == '-') { if (ch >= (unsigned char)set[k] && ch <= (unsigned char)set[k + 2]) in_set = true; k += 2; }
                    else if (ch == (unsigned char)set[k]) in_set = true;
                }
                if (in_set == neg) break;
                if (d) *d++ = (char)ch;
                n++; in_next(in);
            }
            if (!n) break;
            if (d) { *d = 0; assigned++; }
        } else break;
    }
    return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap) { In in = { s, NULL, -2, 0 }; return vscan(&in, fmt, ap); }
int sscanf(const char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }
int fscanf(FILE *f, const char *fmt, ...)
{
    In in = { NULL, f, -2, 0 };
    va_list ap; va_start(ap, fmt); int r = vscan(&in, fmt, ap); va_end(ap);
    in_done(&in);
    return r;
}

char *fgets(char *s, int n, FILE *f)
{
    int k = 0;
    while (k + 1 < n) {
        int c = getc(f);
        if (c == EOF) break;
        s[k++] = (char)c;
        if (c == '\n') break;
    }
    if (!k) return NULL;
    s[k] = 0;
    return s;
}

/* ---------------------------------------------------------------- fmemopen */
typedef struct { unsigned char *buf; size_t size, len, pos; bool owned; } Mem;

static size_t mem_read(FILE *f, unsigned char *d, size_t n)
{
    Mem *m = f->cookie;
    if (m->pos >= m->len) { f->flags |= F_EOF; return 0; }
    if (n > m->len - m->pos) n = m->len - m->pos;
    memcpy(d, m->buf + m->pos, n); m->pos += n;
    return n;
}
static size_t mem_write(FILE *f, const unsigned char *s, size_t n)
{
    Mem *m = f->cookie;
    if (m->pos >= m->size) return 0;
    if (n > m->size - m->pos) n = m->size - m->pos;
    memcpy(m->buf + m->pos, s, n); m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    return n;
}
static off_t mem_seek(FILE *f, off_t off, int whence)
{
    Mem *m = f->cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->pos : (off_t)m->len;
    if (base + off < 0 || (size_t)(base + off) > m->size) return -1;
    m->pos = (size_t)(base + off);
    return (off_t)m->pos;
}
static int mem_close(FILE *f)
{
    Mem *m = f->cookie;
    if (m->owned) free(m->buf);
    free(m); free(f);
    return 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    FILE *f = calloc(1, sizeof *f); Mem *m = calloc(1, sizeof *m);
    if (!f || !m) { free(f); free(m); return NULL; }
    m->buf = buf; m->size = size;
    if (!buf) { m->buf = calloc(1, size ? size : 1); m->owned = true; if (!m->buf) { free(f); free(m); return NULL; } }
    m->len = mode[0] == 'r' ? size : 0;
    f->fd = -1; f->cookie = m;
    f->read = mem_read; f->write = mem_write; f->seek = mem_seek; f->close = mem_close;
    return f;
}
