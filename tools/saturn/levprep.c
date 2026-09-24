/* levprep: the demo's level blocks ("LEVL") as the game reads them, for tools/saturn/layers.py (the Saturn's layer
 * planner). Runs the game's own pack.c, so the block is the decompressed one the game gets.
 *   levprep <data_dir> <out_dir> <id>...   -> <out>/<ID>.levl (the raw block)
 * Build: cc -O2 -std=gnu11 -Isrc tools/saturn/levprep.c src/pack.c src/lzo1z.c -o levprep */
#include "pack.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>

const char *plat_getenv(const char *name) { return getenv(name); }
uint64_t plat_ticks_ms(void) { return 0; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: levprep <data_dir> <out_dir> <id>...\n"); return 2; }
    static const char *const PACKS[] = { "pack.pck", "common.pck", "levels.pck", "menu.pck", "level1.pck" };
    if (!packs_open(argv[1], PACKS, 5)) return 1;
    for (int i = 3; i < argc; i++) {
        uint32_t id = (uint32_t)strtoul(argv[i], NULL, 16);
        const PackEntry *e = packs_find(id);
        if (!e) { fprintf(stderr, "levprep: %08X not found\n", id); return 1; }
        char path[1024]; snprintf(path, sizeof path, "%s/%08X.levl", argv[2], id);
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(e->data, 1, e->size, f) != e->size) { perror(path); return 1; }
        fclose(f);
    }
    return 0;
}
