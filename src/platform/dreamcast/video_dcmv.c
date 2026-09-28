/* video.h on the Dreamcast: videos converted to ZAMV5 (third_party/zamv, from KOS's zamv5-sh4zam-r8 example) by
 * tools/dc/build_disc.py, decoded with SH4ZAM acceleration and shown through the PVR's hardware YUV converter
 * (PVR_TA_YUV_CONV, one Store Queue gather per macroblock row). The decoder itself has no notion of audio: a video's
 * soundtrack is a separate .vsnd sidecar (mono AICA ADPCM, the same shape as a snd.pck sample block) streamed by
 * aud_dc.c's own dedicated channel, and playback paces its frames against that stream's clock, falling back to a
 * wall-clock accumulator for silent clips (the power-attack animations). Pack videos are /cd/video/<ID>.zamv(+.vsnd),
 * our clips the same path as the clip with those extensions.
 *
 * Three threads per video:
 *  - a read-ahead thread keeps a few 64 KB chunks of the file in RAM, so nothing else waits for the drive;
 *  - a decode thread, below the game's priority, decodes into a small ring of YUV420 frames in RAM. It runs while the
 *    game loop waits for the PVR, so a busy scene's frame (40-50 ms of SH-4 time at the intro's bit rate) never holds
 *    up the game's frame, and the ring soaks up the busy stretches;
 *  - the game thread only sends the frame that is due to the YUV converter (about a millisecond of store queue
 *    bursts), from rdc_frame_begin() right after pvr_wait_ready(). A render start (the STARTRENDER write KOS makes
 *    from its list-done / vblank interrupts) resets the converter to the top-left of its texture: one landing in the
 *    middle of a frame's macroblocks wrote the rest of them over the top rows (a torn band at the top of the picture).
 *    Once pvr_wait_ready() returns, the previous scene's render has been started and the next one can't start before
 *    this frame's pvr_scene_finish(), so nothing resets the converter under us there.
 *
 * SABER_VIDLOG=1 (saber.env) keeps each shown frame's timing and prints it when the video closes. */
#include "../../video.h"
#include "pvr_internal.h"
#include "../plat.h"
#include <zamv.h>
#include <zamv_dc_helpers.h>
#include <kos.h>
#include <dc/sq.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* aud_dc.c: the video's own dedicated AICA ADPCM channel (see its top-of-file comment) */
bool     aud_video_audio_open(const char *path);
void     aud_video_audio_close(void);
uint32_t aud_video_audio_ms(void);
void     aud_music_settle(void);   /* the music worker has carried out every request (a stream about to take its own) */

/* The read-ahead and decode threads only run while the game thread waits (for the PVR, mostly). PRIO_MAX, not just
 * below PRIO_DEFAULT: KOS ages a thread that has waited 128 ms up to a better priority than the game's, and the decoder
 * waiting on a full ring did, then took a whole 10 ms timeslice out of the frame it woke in. PRIO_MAX is never aged
 * (and the idle thread sits below it). */
#define WORKER_PRIO PRIO_MAX

/* ---- read-ahead ----
 * Its buffers must be 32-byte aligned: KOS's iso9660 only streams a read straight into the caller's buffer by DMA
 * when it is, and otherwise goes one 2 KB sector at a time through its cache - a drive command per sector, which is
 * what made a 64 KB refill take up to half a second and the videos stall. */
#define PF_SLOTS 4
#define PF_CHUNK_BYTES (64 * 1024)

typedef struct {
    file_t fd;
    uint8_t *buf[PF_SLOTS];
    size_t got[PF_SLOTS];
    semaphore_t free_slots, full_slots;
    int next_fill, next_drain;   /* fill: worker-owned; drain: consumer-owned */
    kthread_t *thd;
    atomic_bool quit;
} Prefetch;

static void *prefetch_worker(void *arg)
{
    Prefetch *p = arg;
    for (;;) {
        sem_wait(&p->free_slots);
        if (atomic_load(&p->quit)) break;
        int i = p->next_fill;
        ssize_t n = fs_read(p->fd, p->buf[i], PF_CHUNK_BYTES);
        p->got[i] = n > 0 ? (size_t)n : 0;   /* 0: end of file (or a read error), which the decoder takes as its end */
        p->next_fill = (i + 1) % PF_SLOTS;
        sem_signal(&p->full_slots);
    }
    return NULL;
}

static int prefetch_read_cb(void *user, void *dst, size_t want, size_t *got)
{
    (void)want;   /* zamv_chunk_reader always asks for exactly its own chunk_bytes, PF_CHUNK_BYTES here */
    Prefetch *p = user;
    int i = p->next_drain;
    sem_wait(&p->full_slots);
    if (atomic_load(&p->quit)) return -1;   /* video_close waking the decoder */
    memcpy(dst, p->buf[i], p->got[i]);
    if (got) *got = p->got[i];
    p->next_drain = (i + 1) % PF_SLOTS;
    sem_signal(&p->free_slots);
    return 0;
}

static void prefetch_stop(Prefetch *p)
{
    atomic_store(&p->quit, true);
    if (p->thd) { sem_signal(&p->free_slots); thd_join(p->thd, NULL); p->thd = NULL; }
    sem_destroy(&p->free_slots); sem_destroy(&p->full_slots);
    for (int i = 0; i < PF_SLOTS; i++) { free(p->buf[i]); p->buf[i] = NULL; }
}

static bool prefetch_start(Prefetch *p, file_t fd)
{
    memset(p, 0, sizeof *p);
    p->fd = fd;
    sem_init(&p->free_slots, PF_SLOTS); sem_init(&p->full_slots, 0);
    for (int i = 0; i < PF_SLOTS; i++) if (!(p->buf[i] = memalign(32, PF_CHUNK_BYTES))) { prefetch_stop(p); return false; }
    if (!(p->thd = thd_create(false, prefetch_worker, p))) { prefetch_stop(p); return false; }
    /* below the game thread too (see WORKER_PRIO): KOS polls the drive with thd_pass() between tries, which at the
     * game's own priority took whole timeslices from its frames. The chunks in hand cover the extra wait. */
    thd_set_prio(p->thd, WORKER_PRIO);
    return true;
}

/* ---- the video ---- */
#define RING_MAX 3                /* decoded frames kept ahead (115 KB each at 320x240) */

struct Video {
    file_t fd;
    Prefetch pf;
    zamv_chunk_reader_t *chunk;
    zamv_decoder_t *dec;
    zamv_file_header_t hdr;
    /* decode thread -> game thread: frame k lives in ring[k % nring] from produced > k until consumed > k */
    zamv_frame_t ring[RING_MAX]; int nring;
    semaphore_t ring_free;
    atomic_int produced;          /* frames decoded (decode thread) */
    int consumed;                 /* frames given back to the decoder (game thread) */
    atomic_bool eof, quit;
    kthread_t *thd;
    /* Double-buffered: the scene before this one (whose render pvr_wait_ready() has just started) may still be
     * reading tex[cur]. */
    pvr_ptr_t tex[2]; size_t tex_bytes; int tw, th, cur;
    uint32_t fmt;
    pvr_poly_hdr_t phdr __attribute__((aligned(32)));
    float u0, v0, u1, v1;
    int w, h;                 /* display size (may differ from the encoded/padded frame size) */
    int shown;                /* index of the frame in the texture, -1 before the first */
    int frames_due;           /* frames the clock says should be up by now (video_update) */
    bool finished, has_audio, started;
    float clock_ms;           /* the wall-clock fallback for videos with no soundtrack */
    /* SABER_VIDLOG: per-frame timing kept in RAM (a serial print per frame would itself stall), dumped at close */
    struct { uint32_t t_ms, frame, due, ready, up_us; } *log; int nlog; uint64_t open_us;
};
#define VIDLOG_MAX 4096

/* open videos, for rdc_frame_begin's upload pass (a menu can hold one while a power clip plays) */
#define MAX_OPEN 4
static Video *open_videos[MAX_OPEN];

static void *decode_worker(void *arg)
{
    Video *v = arg;
    for (;;) {
        sem_wait(&v->ring_free);
        if (atomic_load(&v->quit)) break;
        int k = atomic_load(&v->produced);
        if (zamv_decoder_decode_cb(v->dec, zamv_chunk_reader_read, v->chunk, &v->ring[k % v->nring]) <= 0) break;
        atomic_store(&v->produced, k + 1);
        genwait_wake_all(&v->produced);
    }
    atomic_store(&v->eof, true);
    genwait_wake_all(&v->produced);
    return NULL;
}

static int pot(int n) { int p = 8; while (p < n) p <<= 1; return p; }

static bool setup_texture(Video *v)
{
    v->tw = pot(v->hdr.width); v->th = pot(v->hdr.height);
    v->tex_bytes = (size_t)v->hdr.width * v->hdr.height * 2;
    v->tex[0] = rdc_vram_alloc(v->tex_bytes);
    v->tex[1] = rdc_vram_alloc(v->tex_bytes);
    if (!v->tex[0] || !v->tex[1]) {
        if (v->tex[0]) rdc_vram_free(v->tex[0], v->tex_bytes);
        if (v->tex[1]) rdc_vram_free(v->tex[1], v->tex_bytes);
        v->tex[0] = v->tex[1] = NULL;
        return false;
    }
    rdc_forget_header();
    v->fmt = PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE;
    PVR_SET(PVR_TEXTURE_MODULO, v->hdr.width / 32);
    v->u0 = 0.5f / v->tw; v->v0 = 0.5f / v->th;
    v->u1 = ((float)v->w - 0.5f) / v->tw;
    v->v1 = ((float)v->h - 0.5f) / v->th;
    v->cur = 0;
    rdc_compile(&v->phdr, v->tex[v->cur], v->fmt, v->tw, v->th, R_BLEND_NONE, true, false, false);
    return true;
}

static void free_video(Video *v)
{
    if (v->thd) {   /* wake the decoder wherever it waits: for a ring slot, or for the read-ahead */
        atomic_store(&v->quit, true); atomic_store(&v->pf.quit, true);
        sem_signal(&v->ring_free); sem_signal(&v->pf.full_slots);
        thd_join(v->thd, NULL);
    }
    prefetch_stop(&v->pf);
    sem_destroy(&v->ring_free);
    for (int i = 0; i < v->nring; i++) zamv_frame_free(&v->ring[i]);
    if (v->dec) zamv_decoder_destroy(v->dec);
    if (v->chunk) zamv_chunk_reader_destroy(v->chunk);
    fs_close(v->fd);
    if (v->tex[0]) {
        rdc_forget_header();
        /* the scene queued or rendering now may still sample either texture */
        if (!rdc_in_frame()) { pvr_wait_ready(); pvr_wait_render_done(); }
        rdc_vram_free(v->tex[0], v->tex_bytes);
        rdc_vram_free(v->tex[1], v->tex_bytes);
    }
    free(v->log);
    free(v);
}

static Video *open_zamv(const char *path, const char *audio_path)
{
    int slot = 0;
    while (slot < MAX_OPEN && open_videos[slot]) slot++;
    if (slot == MAX_OPEN) { printf("video: %s: too many open videos\n", path); return NULL; }
    file_t fd = fs_open(path, O_RDONLY);
    if (fd < 0) { printf("video: %s missing\n", path); return NULL; }
    Video *v = memalign(32, sizeof *v);
    if (!v) { fs_close(fd); return NULL; }
    memset(v, 0, sizeof *v);
    v->fd = fd; v->shown = -1;
    sem_init(&v->ring_free, 0);
    if (!prefetch_start(&v->pf, fd)) { printf("video: %s: no read-ahead thread\n", path); goto fail; }
    v->chunk = zamv_chunk_reader_create(prefetch_read_cb, &v->pf, PF_CHUNK_BYTES);
    v->dec = zamv_decoder_create();
    if (!v->chunk || !v->dec || zamv_decoder_read_header_cb(v->dec, zamv_chunk_reader_read, v->chunk, &v->hdr) < 0) {
        printf("video: %s: bad ZAMV stream\n", path);
        goto fail;
    }
    if ((v->hdr.width & 31u) || (v->hdr.height & 15u)) {
        printf("video: %s: %ux%u not a multiple of 32x16\n", path, v->hdr.width, v->hdr.height);
        goto fail;
    }
    v->w = v->hdr.width; v->h = v->hdr.height;
    /* the ring: as deep as memory allows, one frame at the least */
    while (v->nring < RING_MAX && zamv_frame_alloc(&v->ring[v->nring], v->hdr.width, v->hdr.height) == 0) v->nring++;
    if (!v->nring) { printf("video: %s: no memory for a frame\n", path); goto fail; }
    if (!setup_texture(v)) goto fail;
    if (plat_getenv("SABER_VIDLOG")) v->log = malloc(VIDLOG_MAX * sizeof *v->log);
    for (int i = 0; i < v->nring; i++) sem_signal(&v->ring_free);
    if (!(v->thd = thd_create(false, decode_worker, v))) { printf("video: %s: no decode thread\n", path); goto fail; }
    thd_set_prio(v->thd, WORKER_PRIO);
    /* the first frame before the soundtrack starts, so the picture doesn't open behind it */
    for (uint64_t end = timer_ms_gettime64() + 500; !atomic_load(&v->produced) && !atomic_load(&v->eof) && timer_ms_gettime64() < end; )
        thd_sleep(2);
    v->open_us = timer_us_gettime64();

    if (audio_path) {
        aud_music_settle();   /* a music stop still under way must let go of its stream first */
        v->has_audio = aud_video_audio_open(audio_path);
    }
    open_videos[slot] = v;
    return v;
fail:
    free_video(v);
    return NULL;
}

Video *video_open(Ren *r, uint32_t id)
{
    (void)r;
    char path[64], audio[64];
    snprintf(path, sizeof path, "/cd/video/%08lX.zamv", (unsigned long)id);
    snprintf(audio, sizeof audio, "/cd/video/%08lX.vsnd", (unsigned long)id);
    file_t probe = fs_open(audio, O_RDONLY);
    bool has_audio = probe >= 0; if (has_audio) fs_close(probe);
    Video *v = open_zamv(path, has_audio ? audio : NULL);
    /* The briefing source is 768x312: the encoder still bakes every pack video down to 320x240, and the room
     * screen is sized from the source video (video_draw scales v->w/v->h to fit, same as the old DCMV path). */
    if (v && id == 0x2FE798C3u) { v->w = 768; v->h = 312; }
    return v;
}

void video_preload_file(const char *path, real fps) { (void)path; (void)fps; }

Video *video_open_file(Ren *r, const char *path, float fps)
{
    (void)r; (void)fps;   /* the rate is in the ZAMV header */
    if (!path) return NULL;
    char p[256]; snprintf(p, sizeof p, "%s", path);
    char *dot = strrchr(p, '.'); if (dot) *dot = 0;
    strncat(p, ".zamv", sizeof p - strlen(p) - 1);
    return open_zamv(p, NULL);   /* our own clips (power attacks) carry no soundtrack */
}

/* one decoded frame into the texture the next scene will draw */
static void upload(Video *v, const zamv_frame_t *f)
{
    int next = 1 - v->cur;
    if (zamv_dc_configure_yuv420(v->tex[next], v->hdr.width, v->hdr.height) < 0) return;
    void *port = sq_lock((void *)PVR_TA_YUV_CONV);
    for (int my = 0; my < v->hdr.height / 16; my++) zamv_dc_sq_send_row_direct(port, f, my);
    sq_wait();
    sq_unlock();
    v->cur = next;
    rdc_compile(&v->phdr, v->tex[v->cur], v->fmt, v->tw, v->th, R_BLEND_NONE, true, false, false);
}

/* rdc_frame_begin, between pvr_wait_ready() and the new scene: see the top of the file. Shows the latest decoded frame
 * that is due; frames the game loop was too slow to show go by unseen. */
void video_dc_upload_due(void)
{
    for (int i = 0; i < MAX_OPEN; i++) {
        Video *v = open_videos[i];
        if (!v || v->finished) continue;
        int target = v->frames_due - 1;
        if (target <= v->shown) continue;
        int produced = atomic_load(&v->produced);
        /* the decoder only runs while this thread waits; if the game loop leaves it no time (a heavy scene under a
         * power clip), nothing new is ready and the picture is two frames behind, give it some rather than let the
         * picture stand still */
        if (produced - 1 <= v->shown && target - v->shown >= 2 && !atomic_load(&v->eof)) {
            int flags = irq_disable();
            if (atomic_load(&v->produced) == produced && !atomic_load(&v->eof)) genwait_wait(&v->produced, "video frame", 20);
            irq_restore(flags);
            produced = atomic_load(&v->produced);
        }
        int k = target < produced - 1 ? target : produced - 1;
        if (k <= v->shown) continue;
        uint64_t t0 = timer_us_gettime64();
        upload(v, &v->ring[k % v->nring]);
        if (v->log && v->nlog < VIDLOG_MAX)
            v->log[v->nlog++] = (typeof(*v->log)){ (uint32_t)((t0 - v->open_us) / 1000), (uint32_t)k, (uint32_t)v->frames_due,
                                                    (uint32_t)produced, (uint32_t)(timer_us_gettime64() - t0) };
        v->shown = k;
        for (; v->consumed <= k; v->consumed++) sem_signal(&v->ring_free);
    }
}

bool video_update(Video *v, float dt)
{
    if (!v || v->finished) return false;
    if (!v->started) { v->started = true; v->clock_ms = 0; }
    float t_ms;
    if (v->has_audio) t_ms = (float)aud_video_audio_ms();
    else { v->clock_ms += dt * 1000.0f; t_ms = v->clock_ms; }
    float fps = (float)v->hdr.fps_num / (float)(v->hdr.fps_den ? v->hdr.fps_den : 1);
    /* frame k (0-based) is up from k frame times in: the first one right away */
    int due = (int)(t_ms * fps * 0.001f) + 1;
    int count = atomic_load(&v->eof) ? atomic_load(&v->produced) : (int)v->hdr.frame_count;   /* 0: not known yet */
    if (count && due > count) due = count;
    v->frames_due = due;
    /* every frame shown and the last one's time over */
    if (count && v->shown + 1 >= count && t_ms * fps * 0.001f >= (float)count) v->finished = true;
    return !v->finished;
}

static void draw(Video *v, float x, float y, float w, float h)
{
    if (!v || !rdc_in_frame() || v->shown < 0) return;
    float X0, Y0, X1, Y1;
    extern void rdc_xform(float x, float y, float *X, float *Y);
    rdc_xform(x, y, &X0, &Y0); rdc_xform(x + w, y + h, &X1, &Y1);
    RdcVert q[4] = { { X0, Y0, 1, v->u0, v->v0, 0xffffffffu, 0 }, { X1, Y0, 1, v->u1, v->v0, 0xffffffffu, 0 },
                     { X1, Y1, 1, v->u1, v->v1, 0xffffffffu, 0 }, { X0, Y1, 1, v->u0, v->v1, 0xffffffffu, 0 } };
    rdc_header(&v->phdr);
    rdc_poly(q, 4);
}

void video_draw(Video *v, Ren *r, int sw, int sh)
{
    (void)r;
    if (!v) return;
    float scale = (float)sw / v->w; if (v->h * scale > sh) scale = (float)sh / v->h;
    draw(v, (sw - v->w * scale) * 0.5f, (sh - v->h * scale) * 0.5f, v->w * scale, v->h * scale);
}
void video_draw_rect(Video *v, Ren *r, float x, float y, float w, float h) { (void)r; draw(v, x, y, w, h); }
void video_size(const Video *v, int *w, int *h) { *w = v ? v->w : 0; *h = v ? v->h : 0; }

void video_close(Video *v)
{
    if (!v) return;
    for (int i = 0; i < MAX_OPEN; i++) if (open_videos[i] == v) open_videos[i] = NULL;
    if (v->log) {
        printf("vidlog: %d frames shown of %d decoded, ring %d (t_ms frame due ready up_us)\n", v->nlog, atomic_load(&v->produced), v->nring);
        for (int i = 0; i < v->nlog; i++)
            printf("vidlog %d %lu %lu %lu %lu %lu\n", i, (unsigned long)v->log[i].t_ms, (unsigned long)v->log[i].frame,
                   (unsigned long)v->log[i].due, (unsigned long)v->log[i].ready, (unsigned long)v->log[i].up_us);
    }
    if (v->has_audio) aud_video_audio_close();
    free_video(v);
}
