#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoder and compressed read-ahead only. The audio worker owns the KOS stream
 * and calls these functions; close joins the reader before releasing its data. */
typedef struct DcAdx DcAdx;
DcAdx *dc_adx_open(const char *path, bool loop);
void dc_adx_close(DcAdx *adx);
unsigned dc_adx_rate(const DcAdx *adx);
unsigned dc_adx_channels(const DcAdx *adx);
size_t dc_adx_read(DcAdx *adx, int16_t *pcm, size_t frames);
bool dc_adx_done(const DcAdx *adx);
