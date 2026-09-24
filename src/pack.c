#include "pack.h"
#include "lzo1z.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PLAT_DREAMCAST
#include <malloc.h>
#endif
#ifdef PLAT_BAKED_ASSETS
#include "platform/dreamcast/dcfmv/lz4_mini.h"
#endif

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
        {"sfx", RES_SFX}, {"music", RES_MUSIC}, {"video", RES_VIDEO},
        {"tex", RES_TEX}, {"sample", RES_SAMPLE}, {"image", RES_IMAGE}, {"file", RES_FILE} };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].k) == n && !memcmp(tab[i].k, s, n)) return tab[i].t;
    return RES_UNKNOWN;
}

/* block buffers are 32-byte aligned (DMA to video / sound memory) */
static bool (*evict_hook)(void);
void packs_set_evict_hook(bool (*hook)(void)) { evict_hook = hook; }

static void *block_alloc_once(size_t n)
{
#ifdef PLAT_DREAMCAST
    return memalign(32, (n + 31) & ~(size_t)31);
#elif defined(_WIN32)
    return malloc(n);   /* no aligned_alloc there (and _aligned_malloc needs its own free); nothing on PC DMAs these */
#else
    return aligned_alloc(32, (n + 31) & ~(size_t)31);
#endif
}

/* out of memory: textures not drawn lately make room (a level's blocks after the menus') */
static void *block_alloc(size_t n)
{
    void *p;
    while (!(p = block_alloc_once(n)))
        if (!evict_hook || !evict_hook()) return NULL;
    return p;
}

static bool read_at(FILE *f, uint32_t off, void *dst, size_t n)
{
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

bool pack_load(Pack *p, const char *path)
{
    memset(p, 0, sizeof *p);
    p->f = fopen(path, "rb");
    if (!p->f) { fprintf(stderr, "pack: cannot open %s\n", path); return false; }
    fseek(p->f, 0, SEEK_END); p->file_size = (size_t)ftell(p->f);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(p->name, sizeof p->name, "%s", base);

    uint8_t head[16];
    if (p->file_size < 0x20 || !read_at(p->f, 0, head, 16) || memcmp(head, "HEADLIST", 8)) { fprintf(stderr, "pack: bad magic %s\n", path); return false; }
    uint32_t dir_off = rd32(head + 8), dir_len = rd32(head + 12);

    /* the entry table runs from 0x10 to a zero id, the directory text follows it */
    if (dir_off <= 0x10 || dir_off > p->file_size) { fprintf(stderr, "pack: bad directory %s\n", path); return false; }
    uint8_t *tab = malloc(dir_off - 0x10);
    if (!tab || !read_at(p->f, 0x10, tab, dir_off - 0x10)) { free(tab); return false; }
    int n = 0;
    for (const uint8_t *e = tab; e + 16 <= tab + (dir_off - 0x10); e += 16, n++) if (e[0] == 0) break;
    p->entries = calloc(n ? n : 1, sizeof(PackEntry));
    p->count = n;
    uint32_t first_off = n ? rd32(tab + 8) : (uint32_t)p->file_size;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = tab + i * 16;
        PackEntry *pe = &p->entries[i];
        pe->id = hex_id((const char *)e);
        pe->off = rd32(e + 8); pe->size = pe->declen = rd32(e + 12);
        uint32_t next = (i + 1 < n) ? rd32(e + 16 + 8) : (uint32_t)p->file_size;
        pe->stored = next - pe->off;
    }
    free(tab);

    /* directory: "type=ID\n" lines, LZO1Z-compressed */
    uint32_t clen = first_off > dir_off ? first_off - dir_off : 0;
    uint8_t *cdir = malloc(clen + 1), *dir = malloc(dir_len + 16);
    int got = -1;
    if (cdir && dir && read_at(p->f, dir_off, cdir, clen)) got = lzo1z_decompress(cdir, clen, dir, dir_len + 16);
    free(cdir);
    if (got < 0) { fprintf(stderr, "pack: directory decompress failed %s\n", path); free(dir); return false; }
    const char *s = (const char *)dir, *end = s + got;
    while (s < end && *s) {
        const char *eq = memchr(s, '=', end - s); if (!eq) break;
        const char *nl = memchr(eq, '\n', end - eq); if (!nl) nl = end;
        if (nl - eq - 1 >= 8) {
            uint32_t id = hex_id(eq + 1);
            size_t tn = eq - s; bool lz4 = tn > 4 && !memcmp(eq - 4, "+lz4", 4);
            for (int i = 0; i < n; i++) if (p->entries[i].id == id) { p->entries[i].type = type_of(s, lz4 ? tn - 4 : tn); p->entries[i].lz4 = lz4; break; }
        }
        s = nl + 1;
    }
    free(dir);
    return true;
}

static unsigned g_reads;
unsigned packs_reads(void) { return g_reads; }

/* read (and decompress) a block */
static bool entry_load(const Pack *p, PackEntry *pe)
{
    if (pe->data) return true;
    static int log = -1; if (log < 0) log = plat_getenv("SABER_READLOG") != NULL;   /* debug: every read from a pack */
    if (log) fprintf(stderr, "pack: read %s %08X (%u KB) at %u ms\n", p->name, pe->id, (unsigned)(pe->stored / 1024), (unsigned)plat_ticks_ms());
    g_reads++;
    uint8_t *raw = block_alloc(pe->stored + 16);
    if (!raw) { fprintf(stderr, "pack: block %08X: no memory for %u bytes\n", pe->id, (unsigned)pe->stored); return false; }
    if (!read_at(p->f, pe->off, raw, pe->stored)) { fprintf(stderr, "pack: block %08X read failed\n", pe->id); free(raw); return false; }
    if (pe->stored == pe->declen) { pe->data = raw; pe->size = pe->declen; pe->owned = true; return true; }
    uint8_t *buf = block_alloc(pe->declen + 16);
    int r = !buf ? -1
#ifdef PLAT_BAKED_ASSETS
          : pe->lz4 ? (lz4_mini_decode(raw, (int)pe->stored, buf, (int)pe->declen, (int)pe->declen) == (int)pe->declen ? (int)pe->declen : -1)   /* stops at declen: padding follows */
#endif
          : lzo1z_decompress(raw, pe->stored, buf, pe->declen + 16);
    free(raw);
    if (r < 0) { fprintf(stderr, buf ? "pack: block %08X decompress failed\n" : "pack: block %08X: no memory to decompress\n", pe->id); free(buf); return false; }
    pe->data = buf; pe->size = (uint32_t)r; pe->owned = true;
    return true;
}

void pack_free(Pack *p)
{
    for (int i = 0; i < p->count; i++) if (p->entries[i].owned) free((void *)p->entries[i].data);
    free(p->entries);
    if (p->f) fclose(p->f);
    memset(p, 0, sizeof *p);
}

static PackEntry *pack_lookup(const Pack *p, uint32_t id)
{
    for (int i = 0; i < p->count; i++) if (p->entries[i].id == id) return &p->entries[i];
    return NULL;
}
static PackEntry *pack_lookup_type(const Pack *p, uint32_t id, ResType t)
{
    for (int i = 0; i < p->count; i++) if (p->entries[i].id == id && p->entries[i].type == t) return &p->entries[i];
    return NULL;
}

const PackEntry *pack_find(const Pack *p, uint32_t id)
{
    PackEntry *e = pack_lookup(p, id);
    return e && entry_load(p, e) ? e : NULL;
}

/* ---- registry ---- */
#define MAX_PACKS 12
static Pack g_packs[MAX_PACKS];
static int g_npacks;

bool packs_open(const char *data_dir, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < g_npacks; j++) if (!strcmp(g_packs[j].name, names[i])) dup = true;
        if (dup || g_npacks == MAX_PACKS) continue;
        char path[512]; snprintf(path, sizeof path, "%s/%s", data_dir, names[i]);
        if (!pack_load(&g_packs[g_npacks], path)) {
            /* the video pack is optional (consoles play converted videos instead) */
            if (!strcmp(names[i], "video.pck")) { pack_free(&g_packs[g_npacks]); continue; }
            return false;
        }
        g_npacks++;
    }
    return true;
}

const PackEntry *packs_peek(uint32_t id)
{
    for (int i = 0; i < g_npacks; i++) { const PackEntry *e = pack_lookup(&g_packs[i], id); if (e) return e; }
    return NULL;
}

const PackEntry *packs_find(uint32_t id)
{
    for (int i = 0; i < g_npacks; i++) { PackEntry *e = pack_lookup(&g_packs[i], id); if (e) return entry_load(&g_packs[i], e) ? e : NULL; }
    return NULL;
}
const PackEntry *packs_peek_type(uint32_t id, ResType t)
{
    for (int i = 0; i < g_npacks; i++) { const PackEntry *e = pack_lookup_type(&g_packs[i], id, t); if (e) return e; }
    return NULL;
}
const PackEntry *packs_find_type(uint32_t id, ResType t)
{
    for (int i = 0; i < g_npacks; i++) { PackEntry *e = pack_lookup_type(&g_packs[i], id, t); if (e) return entry_load(&g_packs[i], e) ? e : NULL; }
    return NULL;
}

void packs_release(uint32_t id)
{
    for (int i = 0; i < g_npacks; i++) {
        PackEntry *e = pack_lookup(&g_packs[i], id);
        if (e) {
            if (e->owned) { free((void *)e->data); e->data = NULL; e->owned = false; }
            return;
        }
    }
}
void packs_release_type(uint32_t id, ResType t)
{
    for (int i = 0; i < g_npacks; i++) {
        PackEntry *e = pack_lookup_type(&g_packs[i], id, t);
        if (e) {
            if (e->owned) { free((void *)e->data); e->data = NULL; e->owned = false; }
            return;
        }
    }
}
void packs_close(void) { for (int i = 0; i < g_npacks; i++) pack_free(&g_packs[i]); g_npacks = 0; }
