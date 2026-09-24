/* Single-precision soft-float for the SH-2 (no FPU), replacing libgcc's fp-bit.c, which unpacks every operand into a
 * struct, normalises bit by bit and spends most of its time in variable shifts (SH-2 has only 1/2/8/16-bit shifts):
 * in the first level-1 profile two thirds of all instructions were fp-bit. Same results as IEEE single precision
 * with round-to-nearest-even (checked bit for bit against the host FPU: tools/saturn/softfloat_test.sh), except that
 * denormals are flushed to zero. Division runs on the SH-2's on-chip divider (DIVU, 64/32 bits in 39 cycles).
 * Floats travel in integer registers in this ABI, so these take and return their bits as uint32_t.
 * Built with SF_HOST_TEST, the functions get an sf_ prefix and plain C division (host test). */
#include <stdint.h>

#ifdef SF_HOST_TEST
#define SF(name) sf_##name
static inline uint32_t divu_64_32(uint64_t n, uint32_t d, uint32_t *rem) { *rem = (uint32_t)(n % d); return (uint32_t)(n / d); }
#else
#define SF(name) name
#define SF_ATTR __attribute__((used, externally_visible))
/* the on-chip divider (signed 64/32; our operands keep the quotient below 2^31). Not reentrant: nothing divides
 * floats in an interrupt handler. */
static inline uint32_t divu_64_32(uint64_t n, uint32_t d, uint32_t *rem)
{
    volatile uint32_t *const dvsr = (volatile uint32_t *)0xFFFFFF00u, *const dvdnth = (volatile uint32_t *)0xFFFFFF10u,
                      *const dvdntl = (volatile uint32_t *)0xFFFFFF14u;
    *dvsr = d; *dvdnth = (uint32_t)(n >> 32); *dvdntl = (uint32_t)n;   /* the low write starts it */
    uint32_t q = *dvdntl;   /* the read waits for the result */
    *rem = *dvdnth;
    return q;
}
#endif
#ifndef SF_ATTR
#define SF_ATTR
#endif

#define SIGN 0x80000000u
#define INF  0x7F800000u
#define QNAN 0x7FC00000u
#define MANT 0x007FFFFFu

/* leading zeros of x (x != 0), and x shifted so that bit 31 is set; constant shifts only */
static inline uint32_t norm32(uint32_t x, int *lz)
{
    int n = 0;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xF0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xC0000000u)) { n += 2; x <<= 2; }
    if (!(x & 0x80000000u)) { n += 1; x <<= 1; }
    *lz = n;
    return x;
}

/* sign | exponent / mantissa from a mantissa m with bit 31 set standing for 1.xxx * 2^(e - 127), rounded to nearest
 * even on the 8 bits below the 24 kept */
static inline uint32_t pack_round(uint32_t sign, int e, uint32_t m)
{
    uint32_t r = m & 0xFF, q = m >> 8;
    if (r > 0x80 || (r == 0x80 && (q & 1))) { q++; if (q == 0x1000000u) { q >>= 1; e++; } }
    if (e >= 255) return sign | INF;
    if (e <= 0) return sign;   /* flush to zero */
    return sign | ((uint32_t)e << 23) | (q & MANT);
}

static uint32_t add_mag(uint32_t a, uint32_t b)   /* |a| >= |b|, the sign of a */
{
    int ea = (int)((a >> 23) & 0xFF), eb = (int)((b >> 23) & 0xFF);
    uint32_t sign = a & SIGN;
    if (ea == 0xFF) {   /* inf or nan: inf - inf is nan */
        if ((a & MANT) == 0 && eb == 0xFF && (b & MANT) == 0 && ((a ^ b) & SIGN)) return QNAN;
        return (a & MANT) ? a | 0x00400000u : a;
    }
    if (ea == 0) return (a & b & SIGN);   /* both zero (denormals are zero): -0 only for -0 + -0 */
    /* mantissas with 6 bits below them (guard, round, sticky room), at bit 29 */
    uint32_t ma = ((a & MANT) | 0x800000u) << 6, mb = eb ? ((b & MANT) | 0x800000u) << 6 : 0;
    int d = ea - eb;
    if (d) {
        if (d > 30) mb = mb ? 1 : 0;
        else { uint32_t lost = mb & ((1u << d) - 1); mb = (mb >> d) | (lost != 0); }
    }
    uint32_t m;
    if (!((a ^ b) & SIGN)) {   /* a sum: the leading one is at bit 29 or 30 */
        m = ma + mb;
        if (m & 0x40000000u) return pack_round(sign, ea + 1, m << 1);
        return pack_round(sign, ea, m << 2);
    }
    m = ma - mb;
    if (!m) return 0;   /* x - x = +0 */
    if (m & 0x20000000u) return pack_round(sign, ea, m << 2);        /* the usual case: at most one bit lost */
    if (m & 0x10000000u) return pack_round(sign, ea - 1, m << 3);
    int lz; m = norm32(m, &lz);   /* cancellation: exact (d <= 1), no sticky bit */
    /* m had its leading one at bit 29 for exponent ea: now at bit 31 */
    return pack_round(sign, ea + 2 - lz, m);
}

SF_ATTR uint32_t SF(__addsf3)(uint32_t a, uint32_t b)
{
    if ((a & ~SIGN) < (b & ~SIGN)) { uint32_t t = a; a = b; b = t; }
    return add_mag(a, b);
}
SF_ATTR uint32_t SF(__subsf3)(uint32_t a, uint32_t b) { return SF(__addsf3)(a, b ^ SIGN); }
SF_ATTR uint32_t SF(__negsf2)(uint32_t a) { return a ^ SIGN; }

SF_ATTR uint32_t SF(__mulsf3)(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & SIGN;
    int ea = (int)((a >> 23) & 0xFF), eb = (int)((b >> 23) & 0xFF);
    if (ea == 0xFF || eb == 0xFF) {
        if ((ea == 0xFF && (a & MANT)) || (eb == 0xFF && (b & MANT))) return QNAN;
        if ((ea == 0xFF && eb == 0) || (eb == 0xFF && ea == 0)) return QNAN;   /* inf * 0 */
        return sign | INF;
    }
    if (ea == 0 || eb == 0) return sign;
    uint64_t p = (uint64_t)((a & MANT) | 0x800000u) * ((b & MANT) | 0x800000u);   /* 2^46 .. 2^48 */
    uint32_t hi = (uint32_t)(p >> 32), lo = (uint32_t)p;
    int e = ea + eb - 127;
    uint32_t m;   /* the product's leading one at bit 31, the bits dropped below it as a sticky bit 0 */
    if (hi & 0x8000u) { m = (hi << 16) | (lo >> 16) | ((lo & 0xFFFFu) != 0); e++; }   /* leading one at bit 47 */
    else m = (hi << 17) | (lo >> 15) | ((lo & 0x7FFFu) != 0);                         /* at bit 46 */
    return pack_round(sign, e, m);
}

SF_ATTR uint32_t SF(__divsf3)(uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & SIGN;
    int ea = (int)((a >> 23) & 0xFF), eb = (int)((b >> 23) & 0xFF);
    if (ea == 0xFF) {
        if ((a & MANT) || eb == 0xFF) return QNAN;
        return sign | INF;
    }
    if (eb == 0xFF) return (b & MANT) ? QNAN : sign;
    if (eb == 0) return ea == 0 ? QNAN : sign | INF;
    if (ea == 0) return sign;
    uint32_t ma = (a & MANT) | 0x800000u, mb = (b & MANT) | 0x800000u;
    int e = ea - eb + 127;
    if (ma < mb) { ma <<= 1; e--; }
    /* (ma << 30) / mb: 2^30 .. 2^31, 7 bits below the 24 kept; the remainder is the sticky bit */
    uint32_t rem, q = divu_64_32((uint64_t)ma << 30, mb, &rem);
    return pack_round(sign, e, (q << 1) | (rem != 0));
}

SF_ATTR uint32_t SF(__floatsisf)(int32_t i)
{
    if (!i) return 0;
    uint32_t sign = i < 0 ? SIGN : 0, u = i < 0 ? 0u - (uint32_t)i : (uint32_t)i;
    int lz; u = norm32(u, &lz);
    return pack_round(sign, 158 - lz, u);
}
SF_ATTR uint32_t SF(__floatunsisf)(uint32_t u)
{
    if (!u) return 0;
    int lz; u = norm32(u, &lz);
    return pack_round(0, 158 - lz, u);
}

SF_ATTR int32_t SF(__fixsfsi)(uint32_t a)
{
    int e = (int)((a >> 23) & 0xFF) - 127;
    if (e < 0) return 0;
    if (e >= 31) return (a & SIGN) ? INT32_MIN : INT32_MAX;
    uint32_t m = (a & MANT) | 0x800000u;
    uint32_t v = e >= 23 ? m << (e - 23) : m >> (23 - e);
    return (a & SIGN) ? -(int32_t)v : (int32_t)v;
}
SF_ATTR uint32_t SF(__fixunssfsi)(uint32_t a)
{
    if (a & SIGN) return 0;
    int e = (int)((a >> 23) & 0xFF) - 127;
    if (e < 0) return 0;
    if (e >= 32) return UINT32_MAX;
    uint32_t m = (a & MANT) | 0x800000u;
    return e >= 23 ? m << (e - 23) : m >> (23 - e);
}

/* comparisons, libgcc's conventions; unordered (nan) gives the value that makes the test false */
static inline int is_nan(uint32_t a) { return (a & ~SIGN) > INF; }
static inline int32_t key(uint32_t a)   /* an integer ordered like the float (-0 == +0; denormals are zero) */
{
    if (!(a & 0x7F800000u)) return 0;
    return (a & SIGN) ? -(int32_t)(a & ~SIGN) : (int32_t)a;
}
static inline int cmp(uint32_t a, uint32_t b) { int32_t x = key(a), y = key(b); return x < y ? -1 : x > y; }
SF_ATTR int SF(__eqsf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? 1 : cmp(a, b); }
SF_ATTR int SF(__nesf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? 1 : cmp(a, b); }
SF_ATTR int SF(__ltsf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? 1 : cmp(a, b); }
SF_ATTR int SF(__lesf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? 1 : cmp(a, b); }
SF_ATTR int SF(__gtsf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? -1 : cmp(a, b); }
SF_ATTR int SF(__gesf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b) ? -1 : cmp(a, b); }
SF_ATTR int SF(__unordsf2)(uint32_t a, uint32_t b) { return is_nan(a) || is_nan(b); }
