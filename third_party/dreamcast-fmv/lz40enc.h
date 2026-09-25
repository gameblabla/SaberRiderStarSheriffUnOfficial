#pragma once
/* LZ40 (`-ewl` WRAM) encoder for host tools (pack_dcmv, tests).
 *
 * Same byte format as src/platform/dreamcast/dcfmv/lz40.h decodes (and the
 * vendored SH-4 reference src/platform/dreamcast/dcfmv/LZ40_dec.asm):
 * [0x40][declen lo/mid/hi] + negated flag bytes + symbols + terminator.
 * Greedy, threshold 3, 4 KB window, 3-byte hash chain (depth 8). Any valid
 * stream decodes; this one favours build speed over CUE's lazy-optimal ratio.
 *
 * Returns the compressed size, or 0 if dstcap is too small.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LZ40ENC_HASH_BITS 16
#define LZ40ENC_HASH_SIZE (1u << LZ40ENC_HASH_BITS)
#define LZ40ENC_HASH_MASK (LZ40ENC_HASH_SIZE - 1)
#define LZ40ENC_DEPTH 8
#define LZ40ENC_MAX_MATCH 0x1010Fu
#define LZ40ENC_THRESHOLD 3

static inline unsigned lz40enc_hash(const uint8_t *p)
{
    /* FNV-ish 3-byte hash, cheap in C */
    unsigned h = (unsigned)p[0] * 131u + (unsigned)p[1];
    h = h * 131u + (unsigned)p[2];
    h ^= h >> 7;
    return h & LZ40ENC_HASH_MASK;
}

static inline size_t lz40enc_matchlen(const uint8_t *a, const uint8_t *b, size_t maxlen)
{
    size_t k = 0;
    /* word-at-a-time would be faster; byte loop is fine for 35 KB frames */
    while (k < maxlen && a[k] == b[k]) k++;
    return k;
}

static inline size_t lz40_encode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap)
{
    size_t need, i = 0, op;
    int *head = NULL, *prev = NULL;
    size_t flg_idx = 0;
    unsigned mask = 0, logical = 0;

    if (srclen > 0xFFFFFF) return 0;
    need = 4 + srclen + (srclen + 7) / 8 + 3;
    if (dstcap < need) return 0;

    dst[0] = 0x40;
    dst[1] = (uint8_t)(srclen & 0xFF);
    dst[2] = (uint8_t)((srclen >> 8) & 0xFF);
    dst[3] = (uint8_t)((srclen >> 16) & 0xFF);
    op = 4;
    if (srclen == 0) {
        if (dstcap < 7) return 0;
        dst[4] = (uint8_t)((-0x80) & 0xFF);
        dst[5] = 0;
        dst[6] = 0;
        return 7;
    }

    head = (int *)malloc(LZ40ENC_HASH_SIZE * sizeof(int));
    prev = (int *)malloc((srclen + 1) * sizeof(int));
    if (!head || !prev) { free(head); free(prev); return 0; }
    for (size_t k = 0; k < LZ40ENC_HASH_SIZE; k++) head[k] = -1;
    for (size_t k = 0; k <= srclen; k++) prev[k] = -1;

    flg_idx = 0; /* 0 = none yet */
    mask = 0;
    logical = 0;

    while (i < srclen) {
        size_t best_len = 0;
        unsigned best_off = 0;
        mask >>= 1;
        if (mask == 0) {
            if (flg_idx) dst[flg_idx] = (uint8_t)((-(int)logical) & 0xFF);
            flg_idx = op++;
            dst[flg_idx] = 0;
            mask = 0x80;
            logical = 0;
        }
        if (i + 2 < srclen) {
            unsigned h = lz40enc_hash(src + i);
            int cand = head[h];
            int depth = 0;
            size_t maxlen = srclen - i;
            if (maxlen > LZ40ENC_MAX_MATCH) maxlen = LZ40ENC_MAX_MATCH;
            while (cand >= 0 && depth < LZ40ENC_DEPTH) {
                size_t off = i - (size_t)cand;
                if (off == 0 || off >= 0x1000) break;   /* chain is newest-first: older only larger */
                /* same 3-byte prefix by construction? hash collision possible: verify */
                if (src[cand] == src[i] && src[cand + 1] == src[i + 1] && src[cand + 2] == src[i + 2]) {
                    size_t ln = 3 + lz40enc_matchlen(src + i + 3, src + (size_t)cand + 3, maxlen - 3);
                    if (ln > best_len) {
                        best_len = ln;
                        best_off = (unsigned)off;
                        if (ln == maxlen) break;
                    }
                }
                cand = prev[cand];
                depth++;
            }
        }
        if (best_len >= LZ40ENC_THRESHOLD) {
            size_t ln = best_len;
            unsigned off = best_off;
            logical |= mask;
            if (ln <= 0xF) {
                dst[op++] = (uint8_t)(((off & 0xF) << 4) | (unsigned)ln);
                dst[op++] = (uint8_t)((off >> 4) & 0xFF);
            } else if (ln <= 0x10F) {
                dst[op++] = (uint8_t)(((off & 0xF) << 4));
                dst[op++] = (uint8_t)((off >> 4) & 0xFF);
                dst[op++] = (uint8_t)((ln - 0x10) & 0xFF);
            } else {
                unsigned v = (unsigned)(ln - 0x110);
                dst[op++] = (uint8_t)(((off & 0xF) << 4) | 1);
                dst[op++] = (uint8_t)((off >> 4) & 0xFF);
                dst[op++] = (uint8_t)(v & 0xFF);
                dst[op++] = (uint8_t)((v >> 8) & 0xFF);
            }
            for (size_t k = 0; k < ln; k++) {
                size_t p = i + k;
                if (p + 2 < srclen) {
                    unsigned h2 = lz40enc_hash(src + p);
                    prev[p] = head[h2];
                    head[h2] = (int)p;
                }
            }
            i += ln;
        } else {
            dst[op++] = src[i];
            if (i + 2 < srclen) {
                unsigned h = lz40enc_hash(src + i);
                prev[i] = head[h];
                head[h] = (int)i;
            }
            i++;
        }
    }
    mask >>= 1;
    if (mask == 0) {
        if (flg_idx) dst[flg_idx] = (uint8_t)((-(int)logical) & 0xFF);
        flg_idx = op++;
        dst[flg_idx] = 0;
        mask = 0x80;
        logical = 0;
    }
    logical |= mask;
    dst[flg_idx] = (uint8_t)((-(int)logical) & 0xFF);
    dst[op++] = 0;
    dst[op++] = 0;
    free(head);
    free(prev);
    return op;
}
