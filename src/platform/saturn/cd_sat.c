/* Files on the CD for the core's stdio (pack.c, assets.c, SABER.ENV): fopen over libyaul's cdfs file list, read-only.
 * Everything the disc builder writes sits in the ISO root under an 8.3 upper-case name, so a path is matched by its
 * last component, ignoring case (fopen("SaberRider/data/levels.pck") opens LEVELS.PCK).
 * Reads go sector by sector through the CD block: whole sectors straight into the caller's buffer when the file
 * position and the buffer allow it, the rest through a one-sector cache per open file. Nothing reads the CD while
 * CD-DA music plays (plan 7.1): stages load everything up front. */
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static cdfs_filelist_t filelist;
static bool fs_ready;
static unsigned long reads_total, bytes_total;

void cd_sat_init(void)
{
    enum { MAX_FILES = 64 };   /* the root holds ~15 files; libyaul's default (-1) allocates 4096 entries, 112 KB */
    cdfs_filelist_entry_t *entries = cdfs_entries_alloc(MAX_FILES);
    cdfs_config_default_set();
    cdfs_filelist_init(&filelist, entries, MAX_FILES);
    cdfs_filelist_root_read(&filelist);
    fs_ready = true;
    printf("cd: %u files\n", (unsigned)filelist.entries_count);
}

static const cdfs_filelist_entry_t *find(const char *path)
{
    if (!fs_ready) return NULL;
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (uint32_t i = 0; i < filelist.entries_count; i++) {
        const cdfs_filelist_entry_t *e = &filelist.entries[i];
        if (e->type != CDFS_ENTRY_TYPE_FILE) continue;
        const char *a = e->name, *b = base;
        while (*a && *a != ';' && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) a++, b++;
        if ((*a == 0 || *a == ';') && *b == 0) return e;
    }
    return NULL;
}

typedef struct {
    fad_t fad;
    uint32_t size, pos;
    int32_t cached;           /* the sector in cache (relative to the file), -1 none */
    uint8_t cache[2048] __attribute__((aligned(4)));
} CdFile;

static bool read_sectors(fad_t fad, void *dst, uint32_t bytes)
{
    reads_total++; bytes_total += bytes;
    return cd_block_sectors_read(fad, dst, bytes) == 0;
}

static size_t cd_read(FILE *f, unsigned char *dst, size_t n)
{
    CdFile *c = f->cookie;
    if (c->pos >= c->size) { f->flags |= F_EOF; return 0; }
    if (n > c->size - c->pos) n = c->size - c->pos;
    size_t done = 0;
    while (done < n) {
        uint32_t sec = c->pos / 2048, off = c->pos % 2048, left = (uint32_t)(n - done);
        if (off == 0 && left >= 2048 && ((uintptr_t)(dst + done) & 1) == 0) {
            uint32_t whole = left & ~2047u;
            if (!read_sectors(c->fad + sec, dst + done, whole)) break;
            done += whole; c->pos += whole;
            continue;
        }
        if (c->cached != (int32_t)sec) {
            if (!read_sectors(c->fad + sec, c->cache, 2048)) break;
            c->cached = (int32_t)sec;
        }
        uint32_t k = 2048 - off; if (k > left) k = left;
        memcpy(dst + done, c->cache + off, k);
        done += k; c->pos += k;
    }
    return done;
}

static off_t cd_seek(FILE *f, off_t off, int whence)
{
    CdFile *c = f->cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)c->pos : (off_t)c->size;
    if (base + off < 0) return -1;
    c->pos = (uint32_t)(base + off);
    return (off_t)c->pos;
}

static size_t cd_write(FILE *f, const unsigned char *s, size_t n) { (void)f; (void)s; (void)n; return 0; }
static int cd_close(FILE *f) { free(f->cookie); free(f); return 0; }

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    if (!path || !mode || mode[0] != 'r') return NULL;
    const cdfs_filelist_entry_t *e = find(path);
    if (!e) return NULL;
    FILE *f = calloc(1, sizeof *f);
    CdFile *c = hw_malloc(sizeof *c);   /* the cache takes 16-bit writes from the CD block's data register */
    if (!f || !c) { free(f); free(c); return NULL; }
    c->fad = e->starting_fad; c->size = (uint32_t)e->size; c->pos = 0; c->cached = -1;
    f->fd = -1; f->cookie = c;
    f->read = cd_read; f->write = cd_write; f->seek = cd_seek; f->close = cd_close;
    return f;
}

void cd_sat_stats(unsigned long *reads, unsigned long *bytes) { *reads = reads_total; *bytes = bytes_total; }
