/* Minimal LZO1Z decompressor (the E2DM "Pack_Crunch" codec). Clean-room port of the
 * public LZO1Z bitstream: same as LZO1X but with M2/M3 offset bits laid out as
 * (t & mask) << 6 | next >> 2. Returns bytes written, or -1 on error. */
#include "lzo1z.h"
#include <string.h>

int lzo1z_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max)
{
    const uint8_t *ip = in, *ie = in + in_len;
    uint8_t *op = out, *oe = out + out_max;
    const uint8_t *m;
    size_t t, last = 0;   /* LZO1Z: M2 codes with (t&0x1f)>=0x1c reuse the last distance */
#define NEED_IN(n)  if ((size_t)(ie - ip) < (size_t)(n)) return -1
#define NEED_OUT(n) if ((size_t)(oe - op) < (size_t)(n)) return -1
#define COPY_MATCH(len) do { if (m < out) return -1; NEED_OUT(len); \
        for (size_t k = 0; k < (len); k++) { op[k] = m[k]; } op += (len); } while (0)

    NEED_IN(1);
    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) goto match_next;      /* short literal, then a match follows */
        NEED_IN(t); NEED_OUT(t);
        memcpy(op, ip, t); op += t; ip += t;
        goto first_literal_run;
    }
    for (;;) {
        NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;
        /* literal run */
        if (t == 0) {
            while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
            t += 15 + *ip++;
        }
        t += 3;
        NEED_IN(t); NEED_OUT(t);
        memcpy(op, ip, t); op += t; ip += t;
first_literal_run:
        NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;
        /* M1 match after a literal run: 3 bytes, distance 0x701 + (t<<6 | next>>2) */
        NEED_IN(1);
        last = (1 + 0x0700) + (t << 6) + (*ip++ >> 2);
        m = op - last;
        COPY_MATCH(3);
        goto match_done;
        for (;;) {
match:
            if (t >= 64) {                     /* M2: len 3..8 */
                if ((t & 0x1f) < 0x1c) {
                    NEED_IN(1);
                    last = 1 + ((t & 0x1f) << 6) + (*ip++ >> 2);
                }
                m = op - last;
                t = (t >> 5) - 1;
            } else if (t >= 32) {              /* M3: len 2+t, 14-bit distance */
                t &= 31;
                if (t == 0) {
                    NEED_IN(1);
                    while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
                    t += 31 + *ip++;
                }
                NEED_IN(2);
                last = 1 + ((size_t)ip[0] << 6) + (ip[1] >> 2);
                m = op - last;
                ip += 2;
            } else if (t >= 16) {              /* M4: distance >= 0x4000, or EOF */
                m = op - ((t & 8) << 11);
                t &= 7;
                if (t == 0) {
                    NEED_IN(1);
                    while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
                    t += 7 + *ip++;
                }
                NEED_IN(2);
                m -= (ip[0] << 6) + (ip[1] >> 2);
                ip += 2;
                if (m == op) return (int)(op - out);     /* end marker */
                m -= 0x4000;
                last = (size_t)(op - m);
            } else {                           /* M1 after a match: 2 bytes */
                NEED_IN(1);
                last = 1 + (t << 6) + (*ip++ >> 2);
                m = op - last;
                COPY_MATCH(2);
                goto match_done;
            }
            COPY_MATCH(t + 2);
match_done:
            t = ip[-1] & 3;   /* LZO1Z: low bits of the last offset byte */
            if (t == 0) break;
match_next:
            NEED_IN(t); NEED_OUT(t);
            for (size_t k = 0; k < t; k++) op[k] = ip[k];
            op += t; ip += t;
            NEED_IN(1);
            t = *ip++;
        }
    }
#undef NEED_IN
#undef NEED_OUT
#undef COPY_MATCH
}
