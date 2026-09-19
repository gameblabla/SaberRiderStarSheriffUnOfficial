#include "pack.h"
#include "lzo1z.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t hex_id(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        v = v << 4 | d;
    }
    return v;
}

static ResType type_of(const char *s, size_t n)
{
    static const struct { const char *k; ResType t; } tab[] = {
        {"font", RES_FONT}, {"sprite", RES_SPRITE}, {"cblock", RES_CBLOCK}, {"data", RES_DATA},
        {"sfx", RES_SFX}, {"music", RES_MUSIC}, {"video", RES_VIDEO} };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].k) == n && !memcmp(tab[i].k, s, n)) return tab[i].t;
    return RES_UNKNOWN;
}

bool pack_load(Pack *p, const char *path)
{
    memset(p, 0, sizeof *p);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pack: cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END); p->file_size = (size_t)ftell(f); rewind(f);
    p->file = malloc(p->file_size);
    if (fread(p->file, 1, p->file_size, f) != p->file_size) { fclose(f); return false; }
    fclose(f);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(p->name, sizeof p->name, "%s", base);

    if (p->file_size < 0x20 || memcmp(p->file, "HEADLIST", 8)) { fprintf(stderr, "pack: bad magic %s\n", path); return false; }
    uint32_t dir_off = rd32(p->file + 8), dir_len = rd32(p->file + 12);

    /* count entries */
    int n = 0;
    for (const uint8_t *e = p->file + 0x10; e + 16 <= p->file + p->file_size; e += 16, n++)
        if (e[0] == 0) break;
    p->entries = calloc(n, sizeof(PackEntry));
    p->count = n;

    /* directory: "type=ID\n" lines, LZO1Z-compressed */
    uint32_t first_off = rd32(p->file + 0x10 + 8);
    uint8_t *dir = malloc(dir_len + 16);
    int got = lzo1z_decompress(p->file + dir_off, first_off - dir_off, dir, dir_len + 16);
    if (got < 0) { fprintf(stderr, "pack: directory decompress failed %s\n", path); free(dir); return false; }

    for (int i = 0; i < n; i++) {
        const uint8_t *e = p->file + 0x10 + i * 16;
        PackEntry *pe = &p->entries[i];
        pe->id = hex_id((const char *)e);
        uint32_t off = rd32(e + 8), declen = rd32(e + 12);
        uint32_t next = (i + 1 < n) ? rd32(e + 16 + 8) : (uint32_t)p->file_size;
        uint32_t complen = next - off;
        if (complen == declen) { pe->data = p->file + off; pe->size = declen; }
        else {
            uint8_t *buf = malloc(declen + 16);
            int r = lzo1z_decompress(p->file + off, complen, buf, declen + 16);
            if (r < 0) { fprintf(stderr, "pack: block %08X decompress failed\n", pe->id); free(buf); return false; }
            pe->data = buf; pe->size = (uint32_t)r; pe->owned = true;
        }
    }
    /* assign types from directory text */
    const char *s = (const char *)dir, *end = s + got;
    while (s < end && *s) {
        const char *eq = memchr(s, '=', end - s); if (!eq) break;
        const char *nl = memchr(eq, '\n', end - eq); if (!nl) nl = end;
        if (nl - eq - 1 >= 8) {
            uint32_t id = hex_id(eq + 1);
            for (int i = 0; i < n; i++) if (p->entries[i].id == id) { p->entries[i].type = type_of(s, eq - s); break; }
        }
        s = nl + 1;
    }
    free(dir);
    return true;
}

void pack_free(Pack *p)
{
    for (int i = 0; i < p->count; i++) if (p->entries[i].owned) free((void *)p->entries[i].data);
    free(p->entries); free(p->file); memset(p, 0, sizeof *p);
}

const PackEntry *pack_find(const Pack *p, uint32_t id)
{
    for (int i = 0; i < p->count; i++) if (p->entries[i].id == id) return &p->entries[i];
    return NULL;
}

/* ---- registry ---- */
#define MAX_PACKS 8
static Pack g_packs[MAX_PACKS];
static int g_npacks;

bool packs_open(const char *data_dir, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < g_npacks; j++) if (!strcmp(g_packs[j].name, names[i])) dup = true;
        if (dup || g_npacks == MAX_PACKS) continue;
        char path[512]; snprintf(path, sizeof path, "%s/%s", data_dir, names[i]);
        if (!pack_load(&g_packs[g_npacks], path)) return false;
        g_npacks++;
    }
    return true;
}

const PackEntry *packs_find(uint32_t id)
{
    for (int i = 0; i < g_npacks; i++) { const PackEntry *e = pack_find(&g_packs[i], id); if (e) return e; }
    return NULL;
}
const PackEntry *packs_find_type(uint32_t id, ResType t)
{
    const PackEntry *e = packs_find(id); return (e && e->type == t) ? e : NULL;
}
void packs_close(void) { for (int i = 0; i < g_npacks; i++) pack_free(&g_packs[i]); g_npacks = 0; }
