#pragma once
/* The demo's sfx blobs (RIFF WAVE, fmt 0xF423 = 4-bit mono ADPCM / 0xF424 = PCM16, LZO1Z-packed data) to PCM16,
 * for backends that mix PCM themselves. */
#include <stdint.h>
#include <stdbool.h>
/* returns malloc'd samples (interleaved), *frames / *channels set; NULL on a bad blob */
int16_t *sfx_decode_pcm(const uint8_t *riff, uint32_t size, int *frames, int *channels);
