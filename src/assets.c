#include "assets.h"
#include "namehash.h"
#include "pack.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD 64   /* zeroed bytes after a file read (decoders may over-read) */

static bool exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return false; fclose(f); return true; }

/* the name under assets/ of an asset path */
static const char *asset_name(const char *path)
{
    const char *best = NULL;
    for (const char *p = path; (p = strstr(p, "assets/")); p++) best = p + 7;
    return best ? best : path;
}
uint32_t asset_key(const char *path) { return path ? namehash(asset_name(path)) : 0; }

#ifdef PLAT_BAKED_ASSETS
static const PackEntry *baked(const char *path, ResType t) { return path ? packs_find_type(asset_key(path), t) : NULL; }
static bool baked_has(const char *name)
{
    uint32_t k = namehash(name);
    return packs_peek_type(k, RES_FILE) || packs_peek_type(k, RES_TEX) || packs_peek_type(k, RES_SAMPLE) || packs_peek_type(k, RES_IMAGE);
}
#endif

const char *asset_path(const char *name)
{
    static char buf[1024];
    if (!name) return NULL;
#ifdef PLAT_BAKED_ASSETS
    if (baked_has(name)) { snprintf(buf, sizeof buf, "%sassets/%s", plat_base_path() ? plat_base_path() : "", name); return buf; }
#endif
    const char *env = plat_getenv("SABER_ASSETS");
    if (env) { snprintf(buf, sizeof buf, "%s/%s", env, name); if (exists(buf)) return buf; }
    snprintf(buf, sizeof buf, "assets/%s", name); if (exists(buf)) return buf;
    const char *base = plat_base_path();
    if (base) {
        snprintf(buf, sizeof buf, "%sassets/%s", base, name); if (exists(buf)) return buf;
        snprintf(buf, sizeof buf, "%s../assets/%s", base, name); if (exists(buf)) return buf;
    }
    return NULL;
}

uint8_t *file_read(const char *path, size_t *size)
{
#ifdef PLAT_BAKED_ASSETS
    const PackEntry *e = baked(path, RES_FILE);   /* "FILE", u32 size, 24 bytes 0, the bytes */
    if (e) {
        uint32_t n = e->data[4] | e->data[5] << 8 | e->data[6] << 16 | (uint32_t)e->data[7] << 24;
        uint8_t *d = n + 32 <= e->size ? malloc((size_t)n + PAD) : NULL;
        if (d) { memcpy(d, e->data + 32, n); memset(d + n, 0, PAD); *size = n; }
        packs_release_type(asset_key(path), RES_FILE);
        return d;
    }
#endif
    FILE *f = path ? fopen(path, "rb") : NULL; if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? (size_t)n + PAD : PAD);
    if (!d) { fclose(f); return NULL; }
    if (n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    memset(d + (n > 0 ? n : 0), 0, PAD);
    *size = n > 0 ? (size_t)n : 0;
    return d;
}

FILE *asset_fopen(const char *path)
{
#ifdef PLAT_BAKED_ASSETS
    size_t n; uint8_t *d = file_read(path, &n);
    if (!d) return NULL;
    FILE *f = fmemopen(NULL, n + 1, "w+");   /* the stream owns its buffer */
    if (f) { fwrite(d, 1, n, f); rewind(f); }
    free(d);
    return f;
#else
    return path ? fopen(path, "r") : NULL;
#endif
}

uint32_t *png_load_rgba(const char *path, int *w, int *h)
{
    if (!path) return NULL;
#ifdef PLAT_BAKED_ASSETS
    /* "RGBA", u16 w, h, the stored rectangle's u16 x, y, w, h (w 0: all of it), 16 bytes 0, its pixels. Only the part
     * the game reads is stored (mode7.png: the floor strip); the rest comes back transparent black. */
    const PackEntry *e = baked(path, RES_IMAGE);
    if (e) {
        const uint8_t *d = e->data;
        *w = d[4] | d[5] << 8; *h = d[6] | d[7] << 8;
        int rx = d[8] | d[9] << 8, ry = d[10] | d[11] << 8, rw = d[12] | d[13] << 8, rh = d[14] | d[15] << 8;
        if (!rw) { rx = ry = 0; rw = *w; rh = *h; }
        bool fits = rx + rw <= *w && ry + rh <= *h && (size_t)rw * rh * 4 + 32 <= e->size;
        uint32_t *px = fits ? (rw == *w && rh == *h ? malloc((size_t)*w * *h * 4) : calloc((size_t)*w * *h, 4)) : NULL;
        if (px) for (int y = 0; y < rh; y++) memcpy(px + (size_t)(ry + y) * *w + rx, d + 32 + (size_t)y * rw * 4, (size_t)rw * 4);
        packs_release_type(asset_key(path), RES_IMAGE);
        return px;
    }
    fprintf(stderr, "%s: no baked pixels (tools/dc/build_disc.py IMAGES)\n", path);
    return NULL;
#endif
    uint32_t *px = plat_image_load_rgba(path, w, h);
    if (!px) fprintf(stderr, "%s: image decode failed\n", path);
    return px;
}
