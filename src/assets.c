#include "assets.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD 64   /* zeroed bytes after a file read (decoders may over-read) */

static bool exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return false; fclose(f); return true; }

const char *asset_path(const char *name)
{
    static char buf[1024];
    if (!name) return NULL;
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

uint32_t *png_load_rgba(const char *path, int *w, int *h)
{
    if (!path) return NULL;
    uint32_t *px = plat_image_load_rgba(path, w, h);
    if (!px) fprintf(stderr, "%s: image decode failed\n", path);
    return px;
}
