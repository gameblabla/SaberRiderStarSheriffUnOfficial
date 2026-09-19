#pragma once
/* E2DM "HEADLIST" pack container. Loads a .pck into memory and resolves blocks by 32-bit id. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum { RES_UNKNOWN, RES_FONT, RES_SPRITE, RES_CBLOCK, RES_DATA, RES_SFX, RES_MUSIC, RES_VIDEO } ResType;

typedef struct {
    uint32_t id;
    ResType  type;
    const uint8_t *data;   /* decompressed (or raw) block */
    uint32_t size;
    bool     owned;        /* data was malloc'd by us (decompressed) */
} PackEntry;

typedef struct {
    char       name[64];
    uint8_t   *file;       /* whole file in memory */
    size_t     file_size;
    PackEntry *entries;
    int        count;
} Pack;

bool  pack_load(Pack *p, const char *path);
void  pack_free(Pack *p);
const PackEntry *pack_find(const Pack *p, uint32_t id);

/* Registry of all loaded packs, looked up by id across packs (like E2DM_Hlevel_Pool). */
bool  packs_open(const char *data_dir, const char *const *names, int n);
const PackEntry *packs_find(uint32_t id);
const PackEntry *packs_find_type(uint32_t id, ResType t);
void  packs_close(void);
uint32_t hex_id(const char *s8);
