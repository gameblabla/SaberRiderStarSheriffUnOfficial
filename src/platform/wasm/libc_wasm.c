/* The C library the core game needs, for wasm32-unknown-unknown (there is none, and no emscripten or SDL here).
 *
 *   - the heap: a first-fit free list over wasm linear memory, which grows a page at a time as it runs out.
 *     malloc is the only thing that can move the memory, so the renderer's framebuffer and the audio ring are
 *     malloc'd once and their addresses stay good (a JS view has to be re-made when memory.grow detaches it);
 *   - <string.h>, stdlib's conversions, qsort, rand (glibc's TYPE_3, so a run draws the same numbers as the PC build
 *     and the other ports, as platform/saturn/libc_sat.c does for the Saturn);
 *   - stdio: vsnprintf with the conversions the core uses (%s %d %u %x %X %c %p, widths, zero padding, precision and
 *     %f / %e / %g, which real.h's RS()/RSG() are), and vsscanf / vfscanf with %d %u %x %s %c, the [%^] scanset and %n
 *     (the atlas files and SABER_SCRIPT). Streams are vfs_wasm.c's; stdout / stderr go to console.log / console.warn
 *     through the log import, so the 105 debug fprintf(stderr) lines land in the browser's console.
 *   - the maths real.h needs in float (r_sin, r_atan2, r_fmod, r_remainder, r_hypot, r_sqrt and the roundings) and the
 *     double set stb_vorbis wants, in libm_wasm.c.
 */
#include "wasm_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ memory */
/* Linear memory only ever grows upwards and is one contiguous range, so the heap is an implicit block list over the
 * whole slab: every block header knows its neighbours by byte offset, which makes free()'s merge O(1) and keeps the
 * list in address order without a free list. malloc first-fits and extends the slab in chunks when nothing fits.
 *
 * A header is {size, free, prev, next} (offsets from heap_lo, +1, 0 = none). The payload starts 16 bytes in, so it is
 * already 16-byte aligned; a stricter alignment (pack.c's aligned_alloc(32, ...)) shifts the payload on and records
 * the shift in the four bytes before the pointer, so free() finds the header in O(1) again. */
extern unsigned char __heap_base;
#define WASM_PAGE 65536
/* Blocks and payloads are 32-byte aligned, not 16. The header records how far the payload was pushed on (a
 * stricter aligned_alloc), and free() recovers the header by stepping back in ALIGN-sized steps - which only
 * works if every block starts on an ALIGN boundary. 32 is also what the game's own pack blocks want
 * (pack.c's aligned_alloc(32, ...), for a DMA-ready block), so a malloc'd block can be handed to one. */
#define ALIGN      32u
/* The header is 32 bytes, not 16: the payload starts after it, and free() finds the header again by way of a
 * shift word in the four bytes *before* the pointer. With a 16-byte header that word would land on the header's
 * own `next` link and break the block list, so the header is padded out to 32 and the shift word has a home of
 * its own at offset 28 - still inside the header, but in padding nothing else reads. */
#define HDR        32u
#define MIN_SPLIT  (HDR + 32u)      /* a tail smaller than this cannot hold a header and a payload */
#define CHUNK      (1u << 20)

typedef struct { uint32_t size, free, prev, next; } Block;

static uint32_t heap_lo, heap_hi;   /* the slab: heap_lo .. one past the last block's end */
static Block *block_at(uint32_t off) { return off ? (Block *)(heap_lo + off - 1) : NULL; }
static uint32_t block_off(const Block *b) { return (uint32_t)((const unsigned char *)b - (const unsigned char *)heap_lo) + 1; }

size_t memory_total(void) { return (size_t)__builtin_wasm_memory_size(0) * WASM_PAGE; }
size_t memory_used(void) { return heap_hi - heap_lo; }
size_t heap_free_bytes(void)
{
    size_t n = 0;
    /* the walk is bounded: a block that does not sit inside the slab, or that is not at least a header long,
     * means the list has been damaged, and the count stops there rather than running off the memory */
    for (Block *b = block_at(heap_lo ? 1 : 0); b; ) {
        uintptr_t a = (uintptr_t)b;
        if (a < heap_lo || a + sizeof(Block) > heap_hi || b->size < HDR + 16u || a + b->size > heap_hi) break;
        if (b->free) n += b->size - HDR;
        uint32_t next = b->next;
        if (!next) break;
        Block *nb = block_at(next);
        if (!nb || (uintptr_t)nb <= a) break;   /* the list must go forwards */
        b = nb;
    }
    return n;
}

/* the game's own pressure valve (gfx.c's r_set_evict_hook): drop a texture and try again */
static bool (*evict_hook)(void);
void set_heap_hook(bool (*hook)(void)) { evict_hook = hook; }

/* A new free block at the top of the slab. memory.grow returns the memory's size in pages *before* the growth,
 * so the new pages begin there: the first call puts the heap above the module's own data and stack (the link puts
 * the stack first with --stack-first, so the two never meet), and every later call lands above the last. */
static bool heap_grow(size_t want)
{
    size_t pages = (want + WASM_PAGE - 1) / WASM_PAGE;
    if (pages < 1) pages = 1;
    size_t was = __builtin_wasm_memory_grow(0, pages);
    if (was == (size_t)-1) return false;
    uintptr_t a = (uintptr_t)was * WASM_PAGE;
    a = (a + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    uint32_t lo = (uint32_t)a, hi = (uint32_t)(a + pages * WASM_PAGE);
    Block *b = (Block *)lo;
    b->size = hi - lo;
    b->free = 1;
    b->prev = 0;
    b->next = 0;
    if (heap_lo) {
        /* append after the last block. heap_lo stays where it was: every offset in the list is relative to it, so
         * moving it would make the whole list unreachable. */
        Block *last = block_at(1);
        while (last && last->next) last = block_at(last->next);
        last->next = block_off(b);
        b->prev = block_off(last);
    } else {
        heap_lo = lo;
    }
    heap_hi = hi;
    return true;
}

static void *heap_alloc(size_t n, size_t align)
{
    if (n == 0) n = 1;
    if (align < ALIGN) align = ALIGN;
    if (align & (align - 1)) return NULL;   /* aligned_alloc wants a power of two */
    size_t need = (n + align - 1 + HDR) & ~(size_t)(ALIGN - 1);
    for (;;) {
        for (Block *b = block_at(heap_lo ? 1 : 0); b; ) {
            uintptr_t a = (uintptr_t)b;
            if (a < heap_lo || a + sizeof(Block) > heap_hi || b->size < HDR + 16u || a + b->size > heap_hi) break;
            Block *nb = block_at(b->next);
            if (!nb) { b = NULL; break; }
            if ((uintptr_t)nb <= a) break;   /* a damaged list must not be followed off the end */
            if (!b->free || b->size < need) { b = nb; continue; }
            if (b->size >= need + MIN_SPLIT) {   /* split: the tail becomes a free block */
                Block *t = (Block *)((unsigned char *)b + need);
                t->size = b->size - need;
                t->free = 1;
                t->prev = block_off(b);
                t->next = b->next;
                b->next = block_off(t);
                if (t->next) block_at(t->next)->prev = block_off(t);
                b->size = need;
            }
            b->free = 0;
            unsigned char *raw = (unsigned char *)b + HDR;
            uintptr_t p = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
            /* the shift word, in the header's padding (see HDR above) */
            *(uint32_t *)((unsigned char *)b + 28) = (uint32_t)(p - (uintptr_t)b);
            return (void *)p;
        }
        if (!heap_grow(need > CHUNK ? need : CHUNK)) {
            /* the module is at its maximum: ask the game to free something (gfx.c drops the texture drawn longest
             * ago) and look again */
            bool (*hook)(void) = evict_hook;
            if (!hook) return NULL;
            evict_hook = NULL;   /* no recursion: what the hook frees may allocate */
            bool freed = hook();
            evict_hook = hook;
            if (!freed) return NULL;
        }
    }
}

static Block *block_of(void *p)
{
    if (!p || !heap_lo) return NULL;
    uintptr_t u = (uintptr_t)p;
    if (u < heap_lo + HDR || u >= heap_hi) return NULL;
    /* back to the header: the shift recorded in the header's padding says how far the payload was pushed, and
     * the step back is a whole number of ALIGNs because every block starts on one */
    uintptr_t back = u - (u - heap_lo) % HDR;
    Block *b = (Block *)(back - HDR);
    uint32_t shift = *(const uint32_t *)((unsigned char *)b + 28);
    if (shift < HDR || shift > 4096 || (uintptr_t)b + shift != u) return NULL;
    if ((unsigned char *)b < (unsigned char *)heap_lo || (unsigned char *)b + b->size > (unsigned char *)heap_hi) return NULL;
    return b;
}

#ifdef SABER_WASM_SELFTEST
/* a walk of the block list, for the bring-up (tools/wasm/smoke.js --heap) */
size_t wasm_heap_walk(unsigned *buf, unsigned cap)
{
    size_t n = 0;
    for (Block *b = block_at(heap_lo ? 1 : 0); b && n + 4 < cap; b = block_at(b->next)) {
        buf[n++] = (unsigned)(uintptr_t)b;
        buf[n++] = b->size;
        buf[n++] = b->free;
        buf[n++] = b->next;
    }
    buf[n] = 0;
    return n;
}
#endif

#ifdef SABER_WASM_SELFTEST
/* the heap's own two predicates, for the stress test in heap_test.c: is this pointer one of ours, and is the
 * block list still walkable? (block_of is static, so the test cannot ask directly) */
bool wasm_ptr_is_heap(void *p) { return block_of(p) != NULL; }
bool wasm_heap_list_ok(void)
{
    for (Block *b = block_at(heap_lo ? 1 : 0); b; ) {
        uintptr_t a = (uintptr_t)b;
        if (a < heap_lo || a + sizeof(Block) > heap_hi || b->size < HDR + 16u || a + b->size > heap_hi) return false;
        Block *nb = block_at(b->next);
        if (!nb) return true;   /* the last block */
        if ((uintptr_t)nb <= a) return false;
        b = nb;
    }
    return heap_lo != 0;
}
#endif

void *malloc(size_t n) { return heap_alloc(n, ALIGN); }
void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    size_t t = n * size;
    void *p = heap_alloc(t ? t : 1, ALIGN);
    if (p) memset(p, 0, t);
    return p;
}
void *aligned_alloc(size_t align, size_t n) { return heap_alloc(n, align); }

void free(void *p)
{
    Block *b = block_of(p);
    if (!b || b->free) return;
    b->free = 1;
    /* merge forwards, then backwards. Each merge has to re-point the surviving block's *other* neighbour at the
     * block that absorbed it, or the next free() walks into the wrong header and merges the wrong sizes. */
    Block *n = block_at(b->next);
    if (n && n->free) { b->size += n->size; b->next = n->next; if (b->next) block_at(b->next)->prev = block_off(b); }
    Block *pv = block_at(b->prev);
    if (pv && pv->free) {
        pv->size += b->size;
        pv->next = b->next;
        if (b->next) block_at(b->next)->prev = block_off(pv);
    }
}

void *realloc(void *old, size_t n)
{
    if (!old) return malloc(n);
    if (n == 0) { free(old); return NULL; }
    Block *b = block_of(old);
    size_t have = b ? b->size - HDR : 0;
    if (have >= n) return old;
    void *p = malloc(n);
    if (!p) return NULL;
    memcpy(p, old, have < n ? have : n);
    free(old);
    return p;
}

/* ------------------------------------------------------------------ string */
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    /* a word at a time once both ends line up: the framebuffer clear and the texture fills are the hot ones */
    if (n >= 16 && ((uintptr_t)p & 3) == 0) {
        uint32_t w = (uint32_t)(unsigned char)c * 0x01010101u;
        while (n && ((uintptr_t)p & 3)) { *p++ = (unsigned char)c; n--; }
        uint32_t *q = (uint32_t *)p;
        size_t words = n / 4;
        for (size_t i = 0; i < words; i++) q[i] = w;
        p += words * 4;
        n -= words * 4;
    }
    while (n--) *p++ = (unsigned char)c;
    return s;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (((uintptr_t)a | (uintptr_t)b) & 3) { while (n--) *a++ = *b++; return d; }
    uint32_t *q = (uint32_t *)a; const uint32_t *r = (const uint32_t *)b;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) q[i] = r[i];
    a += words * 4; b += words * 4;
    n -= words * 4;
    while (n--) *a++ = *b++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a == b || n == 0) return d;
    if (a < b) return memcpy(d, s, n);
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) { if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b; if (!*a) return 0; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncat(char *d, const char *s, size_t n)
{
    char *r = d;
    while (*d) d++;
    while (n-- && *s) *d++ = *s++;
    *d = 0;
    return r;
}
char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) break; }
    return (char *)last;
}
char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, strlen(n))) return (char *)h;
    return NULL;
}
char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
/* strtok_r kept to what the core's one call site (menu.c, "key = value" lines) needs */
static char *tok_save;
char *strtok(char *s, const char *delim)
{
    if (s) tok_save = s;
    if (!tok_save) return NULL;
    while (*tok_save && strchr(delim, *tok_save)) tok_save++;
    if (!*tok_save) { tok_save = NULL; return NULL; }
    char *start = tok_save;
    while (*tok_save && !strchr(delim, *tok_save)) tok_save++;
    if (*tok_save) *tok_save++ = 0;
    return start;
}

/* ------------------------------------------------------------------ stdlib */
int abs(int v) { return v < 0 ? -v : v; }

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0;
    const char *start = p;
    for (;; p++) {
        int c = (unsigned char)*p;
        int d = isdigit(c) ? c - '0' : isalpha(c) ? tolower(c) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? (unsigned long)-(long)v : v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoul(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* strtod / strtof: a decimal (or 0x hex) significand with an optional exponent, rounded once at the end. Good to
 * about a ulp or two, which is all the core's r_parse and the atlas files want. */
static double str_to_double(const char *s, char **end, bool single)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    double mant = 0;
    int exp10 = 0, any = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        for (p += 2; isxdigit((unsigned char)*p); p++) {
            int c = tolower((unsigned char)*p);
            mant = mant * 16.0 + (double)(isdigit(c) ? c - '0' : c - 'a' + 10);
            any = 1; exp10 += 4;
        }
    } else {
        for (; isdigit((unsigned char)*p); p++) { mant = mant * 10.0 + (double)(*p - '0'); any = 1; }
        if (*p == '.') {
            for (p++; isdigit((unsigned char)*p); p++) { mant = mant * 10.0 + (double)(*p - '0'); any = 1; exp10--; }
        }
        if (any && (*p == 'e' || *p == 'E')) {
            const char *save = p;
            p++;
            bool eneg = false;
            if (*p == '+' || *p == '-') eneg = *p++ == '-';
            if (isdigit((unsigned char)*p)) {
                int e = 0;
                for (; isdigit((unsigned char)*p); p++) { if (e < 100000) e = e * 10 + (*p - '0'); }
                exp10 += eneg ? -e : e;
            } else p = save;
        }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (end) *end = (char *)p;
    double scale = 1.0;
    int e = exp10;
    if (e > 0) { if (e > 400) e = 400; for (int i = 0; i < e; i++) scale *= 10.0; }
    else { if (e < -400) e = -400; for (int i = 0; i < -e; i++) scale *= 0.1; }
    double v = mant * scale;
    if (single) v = (double)(float)v;
    return neg ? -v : v;
}
double strtod(const char *s, char **end) { return str_to_double(s, end, false); }
float strtof(const char *s, char **end) { return (float)str_to_double(s, end, true); }
double atof(const char *s) { return strtod(s, NULL); }

/* rand: glibc's TYPE_3 additive feedback (r[n] = r[n-31] + r[n-3]), the same generator platform/saturn/libc_sat.c
 * reimplements, so the same input script draws the same numbers on every port */
static int32_t rnd_r[34];
static int rnd_i = -1;
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
    int32_t v = (int32_t)((uint32_t)rnd_r[(rnd_i - 31) % 34] + (uint32_t)rnd_r[(rnd_i - 3) % 34]);
    rnd_r[rnd_i % 34] = v;
    rnd_i = (rnd_i + 1) % 34 + 34;
    return (int)((uint32_t)v >> 1);
}

static void swap_bytes(char *a, char *b, size_t n)
{
    if (!(((uintptr_t)a | (uintptr_t)b | n) & 3)) {
        uint32_t *x = (uint32_t *)a, *y = (uint32_t *)b;
        for (n >>= 2; n--; ) { uint32_t t = *x; *x++ = *y; *y++ = t; }
        return;
    }
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}
static void sort_range(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    while (n > 16) {
        char *lo = base, *mid = base + (n / 2) * sz, *hi = base + (n - 1) * sz;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, sz);
        if (cmp(hi, mid) < 0) { swap_bytes(hi, mid, sz); if (cmp(mid, lo) < 0) swap_bytes(mid, lo, sz); }
        swap_bytes(mid, hi - sz, sz);
        char *piv = hi - sz, *i = lo, *j = piv;
        for (;;) {
            do i += sz; while (cmp(i, piv) < 0);
            do j -= sz; while (j > lo && cmp(piv, j) < 0);
            if (i >= j) break;
            swap_bytes(i, j, sz);
        }
        swap_bytes(i, piv, sz);
        size_t left = (size_t)(i - base) / sz, right = n - left - 1;
        if (left < right) { sort_range(base, left, sz, cmp); base = i + sz; n = right; }
        else { sort_range(i + sz, right, sz, cmp); n = left; }
    }
    for (size_t k = 1; k < n; k++)
        for (char *p = base + k * sz; p > base && cmp(p - sz, p) > 0; p -= sz) swap_bytes(p - sz, p, sz);
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    if (n > 1 && size) sort_range(base, n, size, cmp);
}

/* ------------------------------------------------------------------ printf */
typedef struct { char *buf; size_t cap, n; FILE *f; } Out;

static void out_ch(Out *o, char c)
{
    if (o->f) { fputc(c, o->f); o->n++; return; }
    if (o->n + 1 < o->cap) o->buf[o->n] = c;
    o->n++;
}
static void out_str(Out *o, const char *s, size_t n)
{
    if (o->f) { for (size_t i = 0; i < n; i++) fputc(s[i], o->f); o->n += n; return; }
    while (n--) out_ch(o, *s++);
}
static void out_pad(Out *o, char c, int n) { while (n-- > 0) out_ch(o, c); }

/* The float conversions. Everything is built in a buffer and handed over in one go, so %g can drop its trailing
 * zeros (C's %g has no trailing zeros and no bare point) without having to un-write them.
 *
 * An earlier version reached %e and %g through snprintf("%.*e"), which re-entered this function and recursed
 * until the stack ran out - so every RSG() (real.h's "%g" helper) and any other %e/%g in the core's debug output
 * killed the module with a trap. The exponent is worked out by hand instead. */

/* the plain %f form into t: the integer part digit by digit, then `prec` decimals from the fraction, rounded with
 * the carry propagated back into the integer part. Returns the length. The game's reals are screen coordinates and
 * angles, so the scaled fraction fits 64 bits; a value past 2^64 takes its digits off the double itself. */
static int fmt_fixed(char *t, double v, int prec)
{
    if (prec < 0) prec = 6;
    int k = 0;
    if (v < 0 || (v == 0 && __builtin_signbit(v))) { t[k++] = '-'; v = -v; }
    if (v >= 18446744073709551616.0) {
        /* past 2^64 the integer part does not fit a uint64: take the digits off the double itself, most
         * significant first. The game's reals never get here, but a stray %f of a huge value should not be
         * undefined behaviour. */
        double w = v;
        int lead = 0;
        while (w >= 10.0 && lead < 380) { w /= 10.0; lead++; }
        t[k++] = (char)('0' + (int)w);
        w -= (int)w;
        for (int i = 0; i < lead; i++) { w *= 10.0; int d = (int)w; t[k++] = (char)('0' + d); w -= (double)d; }
        if (prec > 0) { t[k++] = '.'; for (int i = 0; i < prec; i++) t[k++] = '0'; }
        return k;
    }
    uint64_t ip = (uint64_t)v;
    double fp = v - (double)ip;
    uint64_t scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    /* the fraction, rounded to nearest with ties to even (as C's printf does) */
    double scaled = fp * (double)scale, fl = (double)(uint64_t)scaled, rem = scaled - fl;
    uint64_t frac;
    if (rem > 0.5) frac = (uint64_t)fl + 1;
    else if (rem < 0.5) frac = (uint64_t)fl;
    else frac = ((uint64_t)fl & 1) ? (uint64_t)fl + 1 : (uint64_t)fl;
    int carry = 0;
    if (scale && frac >= scale) { frac -= scale; carry = 1; }
    char ib[24];
    int m = 0;
    do {
        int d = (int)(ip % 10) + carry;
        carry = d > 9;
        ib[m++] = (char)('0' + (carry ? d - 10 : d));
        ip /= 10;
    } while (ip);
    if (carry) ib[m++] = '1';
    for (int i = m - 1; i >= 0; i--) t[k++] = ib[i];
    if (prec > 0) {
        t[k++] = '.';
        char fb[48];
        int fk = 0;
        for (int i = 0; i < prec; i++) { fb[fk++] = (char)('0' + (int)(frac % 10)); frac /= 10; }
        for (int i = fk - 1; i >= 0; i--) t[k++] = fb[i];   /* most significant first */
    }
    return k;
}

/* the decimal exponent of a non-negative, finite value: a loop, since the values formatted here are screen
 * coordinates, angles and small counts (no denormals, no 1e300 magnitudes) */
static int dec_exponent(double a)
{
    if (a == 0.0) return 0;
    int e = 0;
    if (a < 1.0) { while (a < 1.0 && e > -320) { a *= 10.0; e--; } }
    else { while (a >= 10.0 && e < 320) { a /= 10.0; e++; } }
    return e;
}

/* %g: drop the fraction's trailing zeros, and the point itself when nothing is left after it */
static int fmt_trim(char *t, int k)
{
    int dot = -1;
    for (int i = 0; i < k; i++) if (t[i] == '.') { dot = i; break; }
    if (dot < 0) return k;
    int e = k;
    while (e > dot + 1 && t[e - 1] == '0') e--;
    if (e == dot + 1) e = dot;      /* nothing but zeros left: the point goes too */
    return e;
}

static int fmt_double(Out *o, double v, char conv, int prec, bool alt)
{
    (void)alt;
    if (__builtin_isnan(v)) { out_str(o, "nan", 3); return 3; }
    if (__builtin_isinf(v)) { const char *s = v < 0 ? "-inf" : "inf"; size_t l = strlen(s); out_str(o, s, l); return (int)l; }
    bool is_g = (conv == 'g' || conv == 'G'), up = (conv == 'G');
    if (is_g) {
        if (prec < 0) prec = 6;
        if (prec == 0) prec = 1;
        int ex = dec_exponent(v < 0 ? -v : v);
        /* %g's precision counts significant digits, so the fraction gets prec-1-ex of them (never negative) */
        if (ex < -4 || ex >= prec) { conv = up ? 'E' : 'e'; prec--; if (prec < 0) prec = 0; }   /* %g's prec-1 fraction digits */
        else { conv = 'f'; prec = prec - 1 - ex; if (prec < 0) prec = 0; }
    }
    if (conv == 'e' || conv == 'E') {
        if (prec < 0) prec = 6;
        bool neg = v < 0 || (v == 0 && __builtin_signbit(v));
        double a = neg ? -v : v;
        int ex = dec_exponent(a);
        double m = a;
        for (int i = 0; i < ex; i++) m /= 10.0;
        for (int i = 0; i > ex; i--) m *= 10.0;
        char t[440];
        int n = 0;
        if (neg) t[n++] = '-';
        n += fmt_fixed(t + n, m, prec);
        if (is_g) n = fmt_trim(t, n);
        out_str(o, t, (size_t)n);
        out_ch(o, conv == 'E' ? 'E' : 'e');
        out_ch(o, ex < 0 ? '-' : '+');
        int ae = ex < 0 ? -ex : ex;
        if (ae >= 100) out_ch(o, (char)('0' + ae / 100));
        out_ch(o, (char)('0' + (ae / 10) % 10));
        out_ch(o, (char)('0' + ae % 10));
        return n + 4;
    }
    char t[440];
    int n = fmt_fixed(t, v, prec);
    if (is_g) n = fmt_trim(t, n);
    out_str(o, t, (size_t)n);
    return n;
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
            fmt++;
            prec = 0;
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
        char c = *fmt;
        if (!c) break;
        char buf[64], sign = 0;
        int n = 0;
        const char *s = buf;
        size_t slen = 0;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng == 2 ? va_arg(ap, long long) : lng == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            sign = v < 0 ? '-' : plus ? '+' : space ? ' ' : 0;
            char t[24];
            int k = 0;
            do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            while (k < prec) t[k++] = '0';
            while (k) buf[n++] = t[--k];
            slen = (size_t)n;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long u = c == 'p' ? (unsigned long long)(uintptr_t)va_arg(ap, void *)
                                  : lng == 2 ? va_arg(ap, unsigned long long) : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned);
            if (lng == -1) u = (unsigned short)u; else if (lng == -2) u = (unsigned char)u;
            unsigned base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char t[24];
            int k = 0;
            do { t[k++] = dig[u % base]; u /= base; } while (u);
            while (k < prec) t[k++] = '0';
            if ((alt && base == 16) || c == 'p') { buf[n++] = '0'; buf[n++] = c == 'X' ? 'X' : 'x'; }
            while (k) buf[n++] = t[--k];
            slen = (size_t)n;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            fmt_double(o, va_arg(ap, double), c, prec, alt || c == 'f' || c == 'F');
            continue;   /* already written, with its own sign and point */
        case 'c': buf[n++] = (char)va_arg(ap, int); slen = 1; break;
        case 's': {
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            slen = (size_t)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            break;
        }
        case 'n': { int *p = va_arg(ap, int *); if (p) *p = (int)o->n; continue; }
        case '%': out_ch(o, '%'); continue;
        default: out_ch(o, '%'); out_ch(o, c); continue;
        }
        int len = (int)slen + (sign ? 1 : 0);
        bool zpad = zero && !left && c != 's' && c != 'c' && !(prec >= 0 && (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X'));
        if (!left && !zpad) out_pad(o, ' ', width - len);
        if (sign) out_ch(o, sign);
        if (zpad) out_pad(o, '0', width - len);
        out_str(o, s, slen);
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
int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) { Out o = { NULL, 0, 0, f }; return vformat(&o, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ scanf */
typedef struct { const char *s; FILE *f; int la; int count; } In;   /* la: the looked-ahead character (-2 none) */

static int in_peek(In *in)
{
    if (in->s) return *in->s ? (unsigned char)*in->s : EOF;
    if (in->la == -2) in->la = fgetc(in->f);
    return in->la;
}
static void in_next(In *in) { if (in->s) { if (*in->s) in->s++; } else in->la = -2; in->count++; }

static int vscan(In *in, const char *fmt, va_list ap)
{
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { while (isspace(in_peek(in))) in_next(in); continue; }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (in_peek(in) != (unsigned char)*fmt) break;
            in_next(in);
            continue;
        }
        fmt++;
        bool skip = false;
        if (*fmt == '*') { skip = true; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { lng = *fmt == 'h' ? -1 : lng + 1; fmt++; }
        char c = *fmt;
        if (c == 'n') { if (!skip) *va_arg(ap, int *) = in->count; continue; }
        if (c != 'c' && c != '[') while (isspace(in_peek(in))) in_next(in);
        if (in_peek(in) == EOF) return assigned ? assigned : EOF;
        if (width <= 0) width = 0x7fffffff;
        char tok[256];
        int n = 0;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            int base = c == 'x' || c == 'X' ? 16 : 10;
            if ((in_peek(in) == '-' || in_peek(in) == '+') && n < width) { tok[n++] = (char)in_peek(in); in_next(in); }
            while (n < width && n < 62) {
                int ch = in_peek(in);
                if (!(base == 16 ? isxdigit(ch) : isdigit(ch))) break;
                tok[n++] = (char)ch;
                in_next(in);
            }
            tok[n] = 0;
            if (!n || (n == 1 && (tok[0] == '-' || tok[0] == '+'))) break;
            if (!skip) {
                long v = (c == 'u' || base == 16) ? (long)strtoul(tok, NULL, base) : strtol(tok, NULL, base);
                if (lng > 0) *va_arg(ap, long *) = v; else if (lng < 0) *va_arg(ap, short *) = (short)v; else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
        } else if (c == 's') {
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width) { int ch = in_peek(in); if (ch == EOF || isspace(ch)) break; if (d) *d++ = (char)ch; n++; in_next(in); }
            if (d) { *d = 0; assigned++; }
        } else if (c == 'c') {
            char *d = skip ? NULL : va_arg(ap, char *);
            if (width == 0x7fffffff) width = 1;
            while (n < width) { int ch = in_peek(in); if (ch == EOF) break; if (d) *d++ = (char)ch; n++; in_next(in); }
            if (n && d) assigned++;
        } else if (c == '[') {
            fmt++;
            bool neg = false;
            if (*fmt == '^') { neg = true; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            size_t setn = (size_t)(fmt - set);
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width) {
                int ch = in_peek(in);
                if (ch == EOF) break;
                bool in_set = false;
                for (size_t k = 0; k < setn; k++) {
                    if (k + 2 < setn && set[k + 1] == '-') { if (ch >= (unsigned char)set[k] && ch <= (unsigned char)set[k + 2]) in_set = true; k += 2; }
                    else if (ch == (unsigned char)set[k]) in_set = true;
                }
                if (in_set == neg) break;
                if (d) *d++ = (char)ch;
                n++;
                in_next(in);
            }
            if (n && d) { *d = 0; assigned++; }
        } else break;
    }
    return assigned;
}
int vsscanf(const char *s, const char *fmt, va_list ap) { In in = { s, NULL, -2, 0 }; return vscan(&in, fmt, ap); }
int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
int fscanf(FILE *f, const char *fmt, ...)
{
    In in = { NULL, f, -2, 0 };
    va_list ap; va_start(ap, fmt);
    int r = vscan(&in, fmt, ap);
    va_end(ap);
    if (in.la >= 0) fseek(f, -1, SEEK_CUR);   /* give the look-ahead back */
    return r;
}

/* ------------------------------------------------------------------ assert */
#ifndef NDEBUG
void wasm_assert_fail(const char *expr, const char *file, int line)
{
    char buf[256];
    int n = snprintf(buf, sizeof buf, "assert(%s) at %s:%d", expr, file, line);
    js_log(1, buf, (uint32_t)(n < 0 ? 0 : n));
    __builtin_trap();
}
#endif

/* ------------------------------------------------------------------ ctype */
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
