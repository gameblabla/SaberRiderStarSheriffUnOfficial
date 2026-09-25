#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *blocks;
    uint32_t sample_count, block_count, sample_pos, block_pos, block_index;
    uint8_t format, bits, per_byte, block_bytes;
    bool sound_ram;             /* SH-2 must access SCSP RAM as 16/32-bit words */
    int32_t hist[3];
    int16_t decoded[16];
} SatAdpDecoder;

bool sat_adp_init(SatAdpDecoder *d, const void *data, size_t size);
void sat_adp_rewind(SatAdpDecoder *d);
bool sat_adp_next(SatAdpDecoder *d, int16_t *sample);
uint32_t sat_adp_samples(const void *data, size_t size);
