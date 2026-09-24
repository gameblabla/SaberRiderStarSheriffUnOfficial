#pragma once
/* Our own (non-demo) assets: recreated sprite sheets, voice lines. Looked up in $SABER_ASSETS, ./assets and
 * <exe dir>/assets, <exe dir>/../assets (the platform's plat_base_path).
 * PLAT_BAKED_ASSETS (Dreamcast): the disc has no assets folder of PNGs and WAVs; tools/dc/build_disc.py baked them
 * into packs (tex.pck textures, snd.pck samples, files.pck the rest: text, level blobs, the RGBA of the images the
 * game reads pixels from), each block under asset_key of its name. asset_path then gives <base>assets/<name> for an
 * asset any of them has, and the functions below read it from there. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* full path of an asset file or NULL if it exists nowhere (static buffer) */
const char *asset_path(const char *name);
/* decode a PNG to RGBA8888 (plat_image_load_rgba); caller frees */
uint32_t *png_load_rgba(const char *path, int *w, int *h);
/* whole file into memory (+64 zero bytes of padding); caller frees */
uint8_t *file_read(const char *path, size_t *size);
/* a text asset opened for reading (fclose it) */
FILE *asset_fopen(const char *path);
/* the id of an asset in the console packs: namehash of its name under assets/ ("forest/far.png") */
uint32_t asset_key(const char *path);
