#pragma once
/* E2DM "HEADLIST" pack container. Opening a .pck reads its directory; a block is read and decompressed the first time
 * it is looked up (a console can't hold the demo's 128 MB of packs) and can be released again. A block's data is
 * 32-byte aligned (a stored block is read straight into it, one read, ready for DMA). */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* the demo's block types, then the ones of the console packs tools/dc/build_disc.py bakes (tex.pck, snd.pck, files.pck):
 * a texture in the console's own format, a sound sample, an image's RGBA pixels, one of our asset files */
typedef enum { RES_UNKNOWN, RES_FONT, RES_SPRITE, RES_CBLOCK, RES_DATA, RES_SFX, RES_MUSIC, RES_VIDEO,
               RES_TEX, RES_SAMPLE, RES_IMAGE, RES_FILE } ResType;

typedef struct {
    uint32_t id;
    ResType  type;
    const uint8_t *data;   /* decompressed (or raw) block, NULL until looked up */
    uint32_t size;
    uint32_t off, stored, declen;   /* where the block is in the file, its stored (maybe compressed) and full size */
    bool     owned;        /* data was malloc'd by us */
} PackEntry;

typedef struct {
    char       name[64];
    FILE      *f;
    size_t     file_size;
    PackEntry *entries;
    int        count;
} Pack;

bool  pack_load(Pack *p, const char *path);
void  pack_free(Pack *p);
const PackEntry *pack_find(const Pack *p, uint32_t id);

/* Registry of all loaded packs, looked up by id across packs (like E2DM_Hlevel_Pool). A found entry has its data.
 * An id may exist once per type (a console texture carries the id of the sprite it was made from). */
bool  packs_open(const char *data_dir, const char *const *names, int n);
const PackEntry *packs_find(uint32_t id);
const PackEntry *packs_find_type(uint32_t id, ResType t);
/* the entry without reading its data (type / size queries) */
const PackEntry *packs_peek(uint32_t id);
const PackEntry *packs_peek_type(uint32_t id, ResType t);
/* drop a block's data (it is read again on the next lookup); pointers into it become invalid */
void  packs_release(uint32_t id);
void  packs_release_type(uint32_t id, ResType t);
void  packs_close(void);
uint32_t hex_id(const char *s8);
