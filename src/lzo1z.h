#pragma once
#include <stdint.h>
#include <stddef.h>
int lzo1z_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max);
