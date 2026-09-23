#pragma once
#include <stdint.h>
#include <stddef.h>
/* MUPS (8-byte header + Ogg with renamed pages / codec id) -> a plain Ogg Vorbis file in memory; caller frees */
uint8_t *mups_to_ogg(const uint8_t *d, size_t n, size_t *out_len);
