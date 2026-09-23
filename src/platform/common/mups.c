/* the packs' MUPS music: Ogg Vorbis pages renamed PssH, codec "2Dream"; restored to plain Ogg */
#include "mups.h"
#include <stdlib.h>
#include <string.h>

static uint32_t ogg_crc_table[256];
static void crc_init(void) { for (uint32_t i = 0; i < 256; i++) { uint32_t r = i << 24; for (int k = 0; k < 8; k++) r = (r & 0x80000000) ? (r << 1) ^ 0x04C11DB7 : r << 1; ogg_crc_table[i] = r; } }
static uint32_t ogg_crc(const uint8_t *p, size_t n) { uint32_t c = 0; for (size_t i = 0; i < n; i++) c = (c << 8) ^ ogg_crc_table[((c >> 24) & 0xff) ^ p[i]]; return c; }

uint8_t *mups_to_ogg(const uint8_t *d, size_t n, size_t *out_len)
{
    if (!ogg_crc_table[1]) crc_init();
    if (n < 8 || memcmp(d, "MUPS", 4)) return NULL;
    uint8_t *out = malloc(n); size_t o = 0, i = 8;
    while (i + 27 <= n && !memcmp(d + i, "PssH", 4)) {
        int nseg = d[i + 26]; size_t plen = 0;
        for (int k = 0; k < nseg; k++) plen += d[i + 27 + k];
        size_t page = 27 + nseg + plen;
        if (i + page > n) break;
        memcpy(out + o, d + i, page);
        memcpy(out + o, "OggS", 4);
        uint8_t *pay = out + o + 27 + nseg;
        /* rename the codec id at every packet start within the page */
        for (size_t st = 0, k = 0; k <= (size_t)nseg; k++) {
            if (st + 7 <= plen && (pay[st] == 1 || pay[st] == 3 || pay[st] == 5) && !memcmp(pay + st + 1, "2Dream", 6)) memcpy(pay + st + 1, "vorbis", 6);
            if (k == (size_t)nseg) break;
            st += d[i + 27 + k];
            if (d[i + 27 + k] == 255) { /* continue within the same packet: skip until a short lacing value */
                while (k + 1 < (size_t)nseg && d[i + 27 + k] == 255) { k++; st += d[i + 27 + k]; }
            }
        }
        memset(out + o + 22, 0, 4);
        uint32_t c = ogg_crc(out + o, page);
        out[o + 22] = c & 0xff; out[o + 23] = (c >> 8) & 0xff; out[o + 24] = (c >> 16) & 0xff; out[o + 25] = c >> 24;
        o += page; i += page;
    }
    *out_len = o;
    return out;
}

