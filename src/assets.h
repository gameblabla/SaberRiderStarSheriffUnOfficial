#pragma once
/* Our own (non-demo) assets: recreated sprite sheets, voice lines. Looked up in $SABER_ASSETS, ./assets and
 * <exe dir>/assets, <exe dir>/../assets. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* full path of an asset file or NULL if it exists nowhere (static buffer) */
const char *asset_path(const char *name);
/* decode a PNG (via libavcodec) to RGBA8888; caller frees */
uint32_t *png_load_rgba(const char *path, int *w, int *h);
/* whole file into memory; caller frees */
uint8_t *file_read(const char *path, size_t *size);
