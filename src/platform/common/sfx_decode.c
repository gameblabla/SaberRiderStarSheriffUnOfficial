#include "sfx_decode.h"
#include "../../lzo1z.h"
#include <stdlib.h>
#include <string.h>

static const int T1[16] = {1,3,5,7,9,11,13,15,-1,-3,-5,-7,-9,-11,-13,-15};
static const int T2[8] = {230,230,230,230,307,409,512,614};

int16_t *sfx_decode_pcm(const uint8_t *d, uint32_t size, int *frames, int *channels)
{
    if (!d || size < 44) return NULL;
    uint16_t fmt = d[20] | d[21] << 8, ch = d[22] | d[23] << 8;
    uint32_t claim = d[40] | d[41] << 8 | d[42] << 16 | (uint32_t)d[43] << 24;
    uint8_t *raw = malloc(claim + 16);
    if (!raw) return NULL;
    int n = lzo1z_decompress(d + 44, size - 44, raw, claim + 16);
    if (n < 0) { memcpy(raw, d + 44, size - 44 < claim ? size - 44 : claim); n = (int)claim; }
    int16_t *pcm;
    if (fmt == 0xF423) {          /* Yamaha ADPCM, mono */
        *frames = n * 2; *channels = 1;
        pcm = malloc((size_t)*frames * 2);
        /* FUN_0045bbe0 decodes in chunks of 0x1ffe0 output bytes (0x7ff8 input bytes, 1.486 s) and restarts the
         * predictor (step 0x7f, sample 0) at every chunk; the encoder did the same, so a decoder that carries the
         * state across the boundary drifts off for the rest of the sample */
        int step = 0x7f, prev = 0, k = 0;
        for (int i = 0; pcm && i < n; i++) {
            if (i % 0x7ff8 == 0) { step = 0x7f; prev = 0; }
            for (int h = 0; h < 2; h++) {
                int nib = h ? (raw[i] >> 4) & 15 : raw[i] & 15;
                int prod = T1[nib] * step;
                /* FUN_0045bbe0: the +7 is added only to a negative product, i.e. prod/8 truncated toward zero.
                 * Rounding away from zero instead biases every step and the signal drifts ~3000 LSB/s into
                 * the rails, which garbled the loud end of the outrider line */
                int v = ((prod >= 0 ? prod : prod + 7) >> 3) + prev;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[k++] = (int16_t)v; prev = v;
                step = (step * T2[nib & 7]) >> 8;
                if (step > 0x6000) step = 0x6000;
                if (step <= 0x7e) step = 0x7f;
            }
        }
    } else {                      /* 0xF424: PCM16 */
        *channels = ch ? ch : 2; *frames = n / 2 / *channels;
        pcm = malloc((size_t)n); if (pcm) memcpy(pcm, raw, n);
    }
    free(raw);
    return pcm;
}
