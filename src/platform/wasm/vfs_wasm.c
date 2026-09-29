/* The file system the core reads the demo's data through, for the WASM port.
 *
 * A browser has no file system, so the page fetches each file the game may ask for and hands it over here with
 * wasm_vfs_add(name, bytes): fopen then finds it by name, and pack.c's fseek/fread/ftell and assets.c's file_read
 * work on it unchanged. Nothing is copied again: the block is malloc'd once by the page's caller and stays put,
 * and fread is a memcpy out of it, which is why the packs can stay whole in memory (the browser's own copy of
 * each file is dropped as soon as it has been handed over).
 *
 * The names are the ones the core builds: "data/<pack>.pck" from packs_open and "assets/<name>" from asset_path.
 * An image (a PNG the page decoded to RGBA) is registered the same way with wasm_vfs_add_image, so
 * plat_image_load_rgba finds it by path; the PNG bytes themselves never reach the module.
 *
 * Writing has nowhere to go: level.c's layer dump and mode7.c's map dump are host debug tools behind SABER_*
 * switches the page does not set, so fopen in a write mode returns a sink FILE that counts the bytes. */
#include "wasm_internal.h"
#include "../plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 256
#define MAX_IMAGES 128

typedef struct {
    char     name[192];
    uint8_t *data;
    uint32_t size;
    bool     writable;   /* the null sink */
    uint32_t written;
} VFile;

static VFile files[MAX_FILES];
static int nfiles;

typedef struct Img Img;   /* defined below (the page's decoded PNGs); streams can read one */
struct WASMFile {
    int      idx;       /* the file in files[], or -1 for an image */
    uint32_t pos;
    bool     writing;
    const Img *img;     /* when idx is -1: the decoded PNG this stream reads (its RGBA bytes) */
};

static struct WASMFile f_stderr = { -1, 0, true, NULL };
static struct WASMFile f_stdout = { -1, 0, true, NULL };
FILE *stderr = &f_stderr;
FILE *stdout = &f_stdout;

/* Console output. The core's fprintf reaches the stream one character at a time, so writing each one through to
 * the host would make a console entry per letter; instead each of stdout / stderr collects into a line buffer
 * that is handed over on the newline (or when it fills), so a line of the core's output is one console entry. */
#define LOG_LINE 512
typedef struct { char buf[LOG_LINE]; int n; } LogBuf;
static LogBuf log_out_buf, log_err_buf;

static void log_flush(LogBuf *b, bool err)
{
    if (b->n <= 0) return;
    js_log(err, b->buf, (uint32_t)b->n);
    b->n = 0;
}
static void log_ch(LogBuf *b, bool err, char c)
{
    if (c == '\n') { log_flush(b, err); return; }
    if (b->n >= LOG_LINE - 1) log_flush(b, err);
    b->buf[b->n++] = c;
}
static void log_write(LogBuf *b, bool err, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) log_ch(b, err, s[i]);
}

/* ---- registering ---- */
int wasm_vfs_add(const char *name, uint8_t *data, uint32_t size)
{
    if (!name || !data) return 0;
    int slot = -1;
    for (int i = 0; i < nfiles; i++)
        if (!files[i].writable && !strcmp(files[i].name, name)) { free(files[i].data); slot = i; break; }
    if (slot < 0) {
        if (nfiles >= MAX_FILES) { free(data); return 0; }
        slot = nfiles++;
        files[slot].writable = false;
    }
    snprintf(files[slot].name, sizeof files[slot].name, "%s", name);
    files[slot].data = data;
    files[slot].size = size;
    return slot;
}
int vfs_count(void) { return nfiles; }
size_t vfs_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < nfiles; i++) n += files[i].size;
    return n;
}

/* ---- images: the page's decoded PNGs, by path ---- */
struct Img { char path[192]; uint32_t w, h; uint32_t *rgba; };
static Img images[MAX_IMAGES];
static int nimages;

int wasm_vfs_add_image(const char *path, uint32_t w, uint32_t h, uint32_t *rgba)
{
    if (!path || !rgba || nimages >= MAX_IMAGES) return 0;
    Img *im = &images[nimages];
    snprintf(im->path, sizeof im->path, "%s", path);
    im->w = w;
    im->h = h;
    im->rgba = rgba;
    return ++nimages;
}

static const Img *image_find(const char *path)
{
    if (!path) return NULL;
    /* asset_path hands back "assets/<name>"; the page registers the same path it fetched */
    for (int i = 0; i < nimages; i++) if (!strcmp(images[i].path, path)) return &images[i];
    return NULL;
}

/* platform/plat.h: the PNG the page decoded, copied out for the caller (assets.c frees it) */
uint32_t *plat_image_load_rgba(const char *path, int *w, int *h)
{
    const Img *im = image_find(path);
    if (!im) return NULL;
    size_t n = (size_t)im->w * im->h;
    uint32_t *px = malloc(n * 4);
    if (!px) return NULL;
    memcpy(px, im->rgba, n * 4);
    if (w) *w = (int)im->w;
    if (h) *h = (int)im->h;
    return px;
}

/* ---- streams ----
 * A read stream is a file the page handed over - or one of the images it decoded. The images live in their own
 * table (they are RGBA, not the file's bytes), so fopen would not find them - and the game's asset lookup
 * (assets.c: asset_path + exists) only asks whether a path is there. Letting fopen see the images too makes a
 * PNG path resolve, while the pixels themselves are read through plat_image_load_rgba above; nothing fread()s a
 * PNG (the decoders all go through png_load_rgba), so the RGBA view below is only ever a fallback. */
static const uint8_t *stream_bytes(const struct WASMFile *f, uint32_t *size)
{
    if (!f || f->writing) return NULL;
    if (f->img) {
        if (size) *size = f->img->w * f->img->h * 4;
        return (const uint8_t *)f->img->rgba;
    }
    if (f->idx < 0 || f->idx >= nfiles) return NULL;
    if (size) *size = files[f->idx].size;
    return files[f->idx].data;
}
FILE *fopen(const char *path, const char *mode)
{
    if (!path) return NULL;
    bool write = mode && (strchr(mode, 'w') || strchr(mode, 'a'));
    int idx = -1;
    const Img *img = NULL;
    if (!write) {
        for (int i = 0; i < nfiles; i++) if (!strcmp(files[i].name, path)) { idx = i; break; }
        if (idx < 0) img = image_find(path);
        if (idx < 0 && !img) return NULL;
    } else if (nfiles < MAX_FILES) {
        /* the sink: nothing is written anywhere, but the debug paths that write still find a stream */
        VFile *v = &files[nfiles];
        v->name[0] = 0;
        v->data = NULL;
        v->size = UINT32_MAX;
        v->writable = true;
        v->written = 0;
        idx = nfiles++;
    }
    struct WASMFile *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->idx = idx;
    f->pos = 0;
    f->writing = write;
    f->img = img;
    return (FILE *)f;
}
int fclose(FILE *f)
{
    if (!f || f == &f_stderr || f == &f_stdout) return 0;
    free(f);
    return 0;
}
size_t fread(void *dst, size_t size, size_t n, FILE *f)
{
    if (!f || size == 0 || n == 0) return 0;
    uint32_t total = 0;
    const uint8_t *data = stream_bytes(f, &total);
    if (!data || f->pos >= total) return 0;
    size_t want = size * n;
    if (want / size != n) want = total - f->pos;   /* the product overflowed: take what is left */
    if (want > total - f->pos) want = total - f->pos;
    memcpy(dst, data + f->pos, want);
    f->pos += (uint32_t)want;
    return want / size;
}
size_t fwrite(const void *src, size_t size, size_t n, FILE *f)
{
    if (!f || f->idx < 0 || f->idx >= nfiles) return 0;
    VFile *v = &files[f->idx];
    if (!f->writing) return 0;
    size_t want = size * n;
    v->written += (uint32_t)want;
    (void)src;
    return n;
}
int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    uint32_t total = 0;
    if (!stream_bytes(f, &total)) {
        /* the writable sink: its "size" is the sentinel, so a caller that seeks still sees a stream */
        if (!f->writing || f->idx < 0 || f->idx >= nfiles) return -1;
        total = UINT32_MAX;
    }
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->pos : (long)total;
    long np = base + off;
    if (np < 0 || (uint32_t)np > total) return -1;
    f->pos = (uint32_t)np;
    return 0;
}
long ftell(FILE *f)
{
    if (!f) return -1;
    if (f->writing) {
        if (f->idx < 0 || f->idx >= nfiles) return -1;
        /* the sink: its "size" is the sentinel, so a caller that seeks to the end still sees a stream */
        return (long)files[f->idx].written;
    }
    uint32_t total = 0;
    if (!stream_bytes(f, &total)) return -1;
    return (long)f->pos;
}
void rewind(FILE *f) { if (f) fseek(f, 0, SEEK_SET); }
int fgetc(FILE *f)
{
    if (!f) return EOF;
    uint32_t total = 0;
    const uint8_t *data = stream_bytes(f, &total);
    if (!data || f->pos >= total) return EOF;
    return data[f->pos++];
}
int getc(FILE *f) { return fgetc(f); }
/* one character of pushback: the stream is seekable, so step back over it (libc_sat.c's scanf does the same) */
int ungetc(int c, FILE *f)
{
    if (!f || c == EOF || f->pos == 0) return EOF;
    f->pos--;
    return c;
}
char *fgets(char *s, int n, FILE *f)
{
    int k = 0;
    while (k + 1 < n) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[k++] = (char)c;
        if (c == '\n') break;
    }
    if (!k) return NULL;
    s[k] = 0;
    return s;
}
int fputc(int c, FILE *f)
{
    if (!f) return EOF;
    if (f == &f_stderr) { log_ch(&log_err_buf, true, (char)c); return c; }
    if (f == &f_stdout) { log_ch(&log_out_buf, false, (char)c); return c; }
    if (f->writing && f->idx >= 0 && f->idx < nfiles) { files[f->idx].written++; return c; }
    return EOF;
}
int fputs(const char *s, FILE *f)
{
    if (!s || !f) return EOF;
    size_t n = strlen(s);
    if (f == &f_stderr) { log_write(&log_err_buf, true, s, n); return (int)n; }
    if (f == &f_stdout) { log_write(&log_out_buf, false, s, n); return (int)n; }
    if (f->writing && f->idx >= 0 && f->idx < nfiles) { files[f->idx].written += (uint32_t)n; return (int)n; }
    return EOF;
}
int fflush(FILE *f)
{
    if (f == &f_stderr) log_flush(&log_err_buf, true);
    else if (f == &f_stdout) log_flush(&log_out_buf, false);
    return 0;
}
/* the buffering mode is not honoured: a console stream wants its lines out as they are written, and a read stream
 * has no buffer at all - which is what the core's reads already do (fread is a memcpy out of the block) */
int setvbuf(FILE *f, char *buf, int mode, size_t size) { (void)f; (void)buf; (void)mode; (void)size; return 0; }
int remove(const char *path) { (void)path; return -1; }
int feof(FILE *f)
{
    if (!f || f->writing) return 0;
    uint32_t total = 0;
    if (!stream_bytes(f, &total)) return 0;
    return f->pos >= total;
}
int ferror(FILE *f) { (void)f; return 0; }
