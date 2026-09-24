/* video.h on the Saturn (plan 6): our own "SCPK" files (tools/saturn/film.py): Cinepak frames at 20 fps or less, each
 * with its share of the soundtrack (16-bit mono PCM), streamed from the CD.
 *
 *   header (big-endian, padded to whole sectors): "SCPK", u16 version 1, u16 header sectors, u16 w, h, u16 fps num,
 *     den, u32 video frames, u32 chunks (the frames, then an audio-only tail when the sound outlasts the picture),
 *     u32 audio rate (0: silent), u16 lead frames, u16 bits (16), u32 largest chunk, u32 chunk size[chunks]
 *   chunk k: u32 video bytes, u32 audio bytes, the Cinepak frame padded to 4 bytes, the samples. Chunk 0's samples
 *     run `lead` frames ahead of its picture (they prime the sound ring), every later chunk's continue from there.
 *
 * The file streams into a ring in work RAM (cd_sat_available: never waiting on the drive). The clock is the sound's
 * (pcm_sat_played; the timer for a silent video). Frames are decoded, in order, into the video surface in VDP1 memory
 * (render_sat.c rsat_video_*) while VDP1 is idle, and drawn as one 16bpp sprite. The decoder writes RGB555 straight
 * from its codebooks, which it keeps converted: a V4 entry is its 2x2 pixels, a V1 entry its 4 pixels doubled. */
#include "../../video.h"
#include "../render.h"
#include "sat_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void rsat_video_draw(const RFRect *dst);   /* render_sat.c */

#define MAX_STRIPS 8
#define RING_BYTES (96 * 1024)
#define PREFILL    (64 * 1024)
#define LATENCY_MS 33                       /* a decoded frame reaches the screen a frame or two after the sound */

typedef struct { uint32_t v4[256][2]; uint32_t v1[256][4]; } Books;   /* per strip */

struct Video {
    FILE *f;
    int w, h, fps_num, fps_den, nframes, nchunks, rate, lead;
    uint32_t *chunk;                        /* chunk sizes */
    uint32_t data_left;                     /* bytes of chunks not yet read from the disc */
    uint8_t *ring; uint32_t rd, wr;         /* bytes consumed / read, since the start */
    uint8_t *scratch; uint32_t max_chunk;   /* a chunk that wraps around the ring, copied straight */
    int next;                               /* the next chunk to play */
    bool started, done, surface;
    uint32_t t0;                            /* the timer at the start (a silent video's clock) */
    Books *books;
    unsigned late;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---------------------------------------------------------------- Cinepak (after ffmpeg's cinepak.c) */
static inline uint16_t yuv(int y, int u, int v)
{
    int r = y + 2 * v, g = y - u / 2 - v, b = y + 2 * u;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3));
}

static void codebook(Books *bk, int v1, int id, const uint8_t *d, const uint8_t *end)
{
    int n = (id & 0x04) ? 4 : 6;
    uint32_t flag = 0, mask = 0;
    for (int i = 0; i < 256; i++) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (d + 4 > end) break;
            flag = be32(d); d += 4; mask = 0x80000000u;
        }
        if (!(id & 0x01) || (flag & mask)) {
            if (d + n > end) break;
            int u = 0, v = 0;
            if (n == 6) { u = (int8_t)d[4]; v = (int8_t)d[5]; }
            uint16_t p0 = yuv(d[0], u, v), p1 = yuv(d[1], u, v), p2 = yuv(d[2], u, v), p3 = yuv(d[3], u, v);
            d += n;
            if (v1) {
                bk->v1[i][0] = (uint32_t)p0 << 16 | p0; bk->v1[i][1] = (uint32_t)p1 << 16 | p1;
                bk->v1[i][2] = (uint32_t)p2 << 16 | p2; bk->v1[i][3] = (uint32_t)p3 << 16 | p3;
            } else {
                bk->v4[i][0] = (uint32_t)p0 << 16 | p1; bk->v4[i][1] = (uint32_t)p2 << 16 | p3;
            }
        }
    }
}

static void vectors(const Books *bk, int id, const uint8_t *d, const uint8_t *end, volatile uint16_t *px, int pitch,
                    int x1, int y1, int x2, int y2, int w, int h)
{
    uint32_t flag = 0, mask = 0;
    const int stride = pitch / 2;   /* in 32-bit words */
    if (x2 > w) x2 = w;
    if (y2 > h) y2 = h;
    for (int y = y1; y < y2; y += 4) {
        volatile uint32_t *row = (volatile uint32_t *)(px + y * pitch);
        for (int x = x1; x < x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (d + 4 > end) return;
                flag = be32(d); d += 4; mask = 0x80000000u;
            }
            if (!(id & 0x01) || (flag & mask)) {
                if (!(id & 0x02) && !(mask >>= 1)) {
                    if (d + 4 > end) return;
                    flag = be32(d); d += 4; mask = 0x80000000u;
                }
                volatile uint32_t *o = row + x / 2;
                if ((id & 0x02) || (~flag & mask)) {   /* V1: one entry, each pixel doubled */
                    if (d >= end) return;
                    const uint32_t *e = bk->v1[*d++];
                    o[0] = e[0]; o[1] = e[1]; o[stride] = e[0]; o[stride + 1] = e[1];
                    o[2 * stride] = e[2]; o[2 * stride + 1] = e[3]; o[3 * stride] = e[2]; o[3 * stride + 1] = e[3];
                } else {                                 /* V4: four entries, a 2x2 each */
                    if (d + 4 > end) return;
                    const uint32_t *a = bk->v4[d[0]], *b = bk->v4[d[1]], *c = bk->v4[d[2]], *e = bk->v4[d[3]];
                    d += 4;
                    o[0] = a[0]; o[1] = b[0]; o[stride] = a[1]; o[stride + 1] = b[1];
                    o[2 * stride] = c[0]; o[2 * stride + 1] = e[0]; o[3 * stride] = c[1]; o[3 * stride + 1] = e[1];
                }
            }
        }
    }
}

static void decode(Video *v, const uint8_t *d, uint32_t len, volatile uint16_t *px, int pitch)
{
    if (len < 10) return;
    const uint8_t *end = d + len;
    int flags = d[0], strips = be16(d + 8);
    d += 10;
    int y0 = 0;
    for (int i = 0; i < strips && d + 12 <= end; i++) {
        uint32_t size = be24(d + 1);
        if (size < 12 || d + size > end) return;
        int y1 = be16(d + 4), x1 = be16(d + 6), y2 = be16(d + 8), x2 = be16(d + 10);
        if (!y1) { y1 = y0; y2 += y1; }   /* "relative to the previous strip" */
        Books *bk = &v->books[i < MAX_STRIPS ? i : MAX_STRIPS - 1];
        if (i > 0 && i < MAX_STRIPS && !(flags & 0x01)) memcpy(bk, &v->books[i - 1], sizeof *bk);
        const uint8_t *s = d + 12, *se = d + size;
        while (s + 4 <= se) {
            int id = s[0]; uint32_t n = be24(s + 1);
            if (n < 4) return;
            const uint8_t *c = s + 4, *ce = s + n > se ? se : s + n;
            if (id >= 0x20 && id <= 0x27) codebook(bk, id & 0x02, id, c, ce);
            else if (id >= 0x30 && id <= 0x32) { vectors(bk, id, c, ce, px, pitch, x1, y1, x2, y2, v->w, v->h); break; }
            s = ce;
        }
        d += size;
        y0 = y2;
    }
}

/* ---------------------------------------------------------------- streaming */
static void fill(Video *v)
{
    while (v->data_left) {
        uint32_t free_bytes = RING_BYTES - (v->wr - v->rd), at = v->wr % RING_BYTES;
        uint32_t n = cd_sat_available(v->f);
        if (n > v->data_left) n = v->data_left;
        if (n > free_bytes) n = free_bytes;
        if (n > RING_BYTES - at) n = RING_BYTES - at;
        if (n < v->data_left && n >= 2048) n &= ~2047u;   /* whole sectors: the reads stay aligned */
        if (!n) return;
        size_t got = fread(v->ring + at, 1, n, v->f);
        v->wr += (uint32_t)got; v->data_left -= (uint32_t)got;
        if (got < n) { v->data_left = 0; return; }
    }
}

/* chunk k's bytes if the ring holds them all (linear: copied to scratch when they wrap), else NULL */
static const uint8_t *chunk_data(Video *v, int k)
{
    uint32_t size = v->chunk[k];
    if (v->wr - v->rd < size) return NULL;
    uint32_t at = v->rd % RING_BYTES;
    if (at + size <= RING_BYTES) return v->ring + at;
    uint32_t first = RING_BYTES - at;
    memcpy(v->scratch, v->ring + at, first); memcpy(v->scratch + first, v->ring, size - first);
    return v->scratch;
}

static const int16_t *samples(const uint8_t *c) { return (const int16_t *)(c + 8 + ((be32(c) + 3) & ~3u)); }

static uint32_t clock_ms(const Video *v)
{
    if (v->rate) return (uint32_t)((uint64_t)pcm_sat_played() * 1000u / (uint32_t)v->rate);
    return (sat_timer_us() - v->t0) / 1000u;
}

/* the chunks due by the clock: sound to the ring, picture into the surface (called while VDP1 is idle) */
static void hook(void *ud, volatile uint16_t *px, int pitch)
{
    Video *v = ud;
    fill(v);
    uint32_t ms = clock_ms(v) + LATENCY_MS;
    int due = (int)((uint64_t)ms * (uint32_t)v->fps_num / (1000u * (uint32_t)v->fps_den)) + 1;
    if (due > v->nframes) due = v->nframes;
    int decoded = 0;
    while (v->next < due) {
        const uint8_t *c = chunk_data(v, v->next);
        if (!c) { v->late++; break; }   /* the disc is behind: the sound plays on, the picture waits */
        uint32_t vlen = be32(c), alen = be32(c + 4);
        if (alen && v->next > 0 && v->rate) pcm_sat_write(samples(c), (int)(alen / 2));
        decode(v, c + 8, vlen, px, pitch);
        v->rd += v->chunk[v->next]; v->next++;
        if (++decoded == 4) break;       /* far behind: the rest next frame */
    }
    if (v->next >= v->nframes) v->done = true;
}

static Video *open_path(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (!f) { printf("video: %s missing\n", name); return NULL; }
    uint8_t head[2048];
    if (fread(head, 1, 2048, f) != 2048 || memcmp(head, "SCPK", 4) || be16(head + 4) != 1) { fclose(f); printf("video: %s: not SCPK\n", name); return NULL; }
    Video *v = calloc(1, sizeof *v);
    if (!v) { fclose(f); return NULL; }
    int sectors = be16(head + 6);
    v->f = f; v->w = be16(head + 8); v->h = be16(head + 10); v->fps_num = be16(head + 12); v->fps_den = be16(head + 14);
    v->nframes = (int)be32(head + 16); v->nchunks = (int)be32(head + 20); v->rate = (int)be32(head + 24);
    v->lead = be16(head + 28); v->max_chunk = be32(head + 32);
    uint8_t *table = malloc((size_t)sectors * 2048);
    v->chunk = malloc((size_t)v->nchunks * 4);
    v->ring = malloc(RING_BYTES);
    v->scratch = malloc(v->max_chunk + 4);
    v->books = calloc(MAX_STRIPS, sizeof *v->books);
    bool ok = table && v->chunk && v->ring && v->scratch && v->books && v->fps_num && v->fps_den && v->max_chunk <= RING_BYTES / 2;
    if (ok) {
        memcpy(table, head, 2048);
        ok = sectors == 1 || fread(table + 2048, 1, (size_t)(sectors - 1) * 2048, f) == (size_t)(sectors - 1) * 2048;
    }
    if (ok) {
        uint32_t total = 0;
        for (int k = 0; k < v->nchunks; k++) { v->chunk[k] = be32(table + 36 + 4 * k); total += v->chunk[k]; }
        v->data_left = total;
    }
    free(table);
    if (!ok) { printf("video: %s: no RAM or a bad header\n", name); video_close(v); return NULL; }
    /* prefill (waiting for the drive once), then the lead of sound and the clock */
    while (v->data_left && v->wr < PREFILL) {
        uint32_t n = v->data_left < PREFILL - v->wr ? v->data_left : PREFILL - v->wr;
        size_t got = fread(v->ring + v->wr, 1, n, f);
        v->wr += (uint32_t)got; v->data_left -= (uint32_t)got;
        if (got < n) { v->data_left = 0; break; }
    }
    const uint8_t *c0 = chunk_data(v, 0);
    if (!c0) { printf("video: %s: truncated\n", name); video_close(v); return NULL; }
    if (v->rate) {
        if (!pcm_sat_start(v->rate, samples(c0), (int)(be32(c0 + 4) / 2))) v->rate = 0;
    }
    v->t0 = sat_timer_us();
    v->surface = rsat_video_open(v->w, v->h, hook, v);
    if (!v->surface) printf("video: %s: no room for a %dx%d surface\n", name, v->w, v->h);
    printf("video: %s %dx%d, %d frames at %d/%d fps, sound %d Hz\n", name, v->w, v->h, v->nframes, v->fps_num, v->fps_den, v->rate);
    return v;
}

Video *video_open(Ren *r, uint32_t id)
{
    (void)r;
    char name[16]; snprintf(name, sizeof name, "%08X.CPK", (unsigned)id);
    return open_path(name);
}

/* one of our clips: its name on the disc is the file's, as .CPK ("power/saber.m4v" -> SABER.CPK) */
Video *video_open_file(Ren *r, const char *path, real fps)
{
    (void)r; (void)fps;
    if (!path) return NULL;
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    char name[16]; int n = 0;
    while (*b && *b != '.' && n < 8) name[n++] = (char)toupper((unsigned char)*b++);
    strcpy(name + n, ".CPK");
    return open_path(name);
}

bool video_update(Video *v, real dt)
{
    (void)dt;
    if (!v) return false;
    fill(v);
    if (!v->surface) return false;
    return !v->done;
}

void video_draw_rect(Video *v, Ren *r, real x, real y, real w, real h)
{
    (void)r;
    if (!v || !v->surface) return;
    RFRect d = { r_floorr(x), r_floorr(y), r_floorr(w), r_floorr(h) };
    rsat_video_draw(&d);
}

void video_draw(Video *v, Ren *r, int sw, int sh)
{
    if (!v) return;
    /* the largest whole-pixel fit, centred: at 1:1 when it fits (the Saturn's clips are made for the screen) */
    int w = v->w, h = v->h;
    if (w > sw || h > sh) { if (w * sh > h * sw) { h = h * sw / w; w = sw; } else { w = w * sh / h; h = sh; } }
    video_draw_rect(v, r, r_int((sw - w) / 2), r_int((sh - h) / 2), r_int(w), r_int(h));
}

void video_size(const Video *v, int *w, int *h)
{
    if (w) *w = v ? v->w : 0;
    if (h) *h = v ? v->h : 0;
}

void video_close(Video *v)
{
    if (!v) return;
    if (v->surface) rsat_video_close();
    if (v->rate) {
        /* played to its end: the sound that outlasts the picture (the tail chunk) plays out; stopped early: silence */
        if (v->done && v->nchunks > v->nframes) {
            while (v->data_left) {   /* the rest of the file: the tail is small */
                uint32_t free_bytes = RING_BYTES - (v->wr - v->rd), at = v->wr % RING_BYTES;
                uint32_t n = v->data_left; if (n > free_bytes) n = free_bytes; if (n > RING_BYTES - at) n = RING_BYTES - at;
                if (!n) break;
                size_t got = fread(v->ring + at, 1, n, v->f);
                v->wr += (uint32_t)got; v->data_left -= (uint32_t)got;
                if (got < n) break;
            }
            const uint8_t *c = v->next < v->nchunks ? chunk_data(v, v->nframes) : NULL;
            if (c && v->next == v->nframes) pcm_sat_finish(samples(c), (int)(be32(c + 4) / 2));
            else pcm_sat_finish(NULL, 0);
        } else if (v->done) pcm_sat_finish(NULL, 0);
        else pcm_sat_stop();
    }
    if (v->late) printf("video: %u frames waited for the disc\n", v->late);
    if (v->f) fclose(v->f);
    free(v->chunk); free(v->ring); free(v->scratch); free(v->books); free(v);
}
