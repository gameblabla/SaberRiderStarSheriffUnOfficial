/* video.h on the Dreamcast: the videos converted to DCMV (VQ-compressed YUV422 textures, LZ4-packed, with an
 * ADPCM soundtrack; Dreamcast/dreamcast-fmv) played by the vendored dcfmv module in client-present mode: its
 * worker thread reads and unpacks frames ahead, video_draw DMAs the current one into its texture and draws a
 * quad through the PVR renderer like any other texture, so the game can put a video in a window (the briefing
 * room screen) or under its own overlays. Pack videos are /cd/video/<ID>.dcmv, our clips the same path as the
 * clip with a .dcmv extension. */
#include "../../video.h"
#include "pvr_internal.h"
#include "dcfmv/dcfmv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aud_music_settle(void);   /* aud_dc.c: the music worker has carried out every request */

struct Video {
    dcfmv_t *fmv;
    pvr_ptr_t tex; size_t tex_bytes; int tw, th;
    pvr_poly_hdr_t hdr __attribute__((aligned(32)));
    float u0, v0, u1, v1;
    int w, h;                  /* source display size (may differ from the converted texture) */
    kthread_t *worker; volatile bool quit;
    bool finished, started, audio_ready;
};

static void *worker(void *p)
{
    Video *v = p;
    while (!v->quit) dcfmv_worker_step(v->fmv);   /* sleeps a tick per step */
    return NULL;
}

static int pot(int n) { int p = 8; while (p < n) p <<= 1; return p; }

static Video *open_path(const char *path)
{
    file_t probe = fs_open(path, O_RDONLY);
    if (probe < 0) { printf("video: %s missing\n", path); return NULL; }
    fs_close(probe);
    Video *v = memalign(32, sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->fmv = dcfmv_create(DCFMV_PRESENT_CLIENT);
    if (!v->fmv) { free(v); return NULL; }
    dcfmv_current = v->fmv;
    dcfmv_control_reset();
    if (dcfmv_open(v->fmv, path) < 0) { dcfmv_destroy(v->fmv); free(v); return NULL; }
    const dcfmv_media_info_t *info = dcfmv_media_info(v->fmv);
    v->w = info->content_width; v->h = info->content_height;
    v->tw = pot(info->tex_width); v->th = pot(info->tex_height);
    bool strided = v->tw != info->tex_width || v->th != info->tex_height;
    v->tex_bytes = (size_t)v->tw * v->th * 2;
    v->tex = rdc_vram_alloc(v->tex_bytes);
    if (!v->tex) { dcfmv_close(v->fmv); dcfmv_destroy(v->fmv); free(v); return NULL; }
    rdc_forget_header();
    uint32_t fmt = (info->frame_type == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) | PVR_TXRFMT_VQ_ENABLE;
    if (strided) {
        /* the packer's strided layout: one global stride register; only videos use it */
        fmt |= PVR_TXRFMT_X32_STRIDE | PVR_TXRFMT_NONTWIDDLED;
        PVR_SET(PVR_TEXTURE_MODULO, info->tex_width / 32);
        v->u0 = 0.5f / v->tw; v->v0 = 0.5f / v->th;
        v->u1 = ((float)info->content_width - 0.5f) / v->tw;
        v->v1 = ((float)info->content_height - 0.5f) / v->th;
    } else {
        fmt |= PVR_TXRFMT_TWIDDLED;
        v->u0 = ((float)(info->tex_width - info->content_width) * 0.5f + 0.5f) / info->tex_width;
        v->v0 = ((float)(info->tex_height - info->content_height) * 0.5f + 0.5f) / info->tex_height;
        v->u1 = 1.0f - v->u0; v->v1 = 1.0f - v->v0;
    }
    rdc_compile(&v->hdr, v->tex, fmt, v->tw, v->th, R_BLEND_NONE, true, false, false);
    dcfmv_set_render_resources(v->fmv, v->tex, &v->hdr, &v->hdr, NULL, NULL);
    dcfmv_reset_render_tracking(v->fmv);

    /* sound: the movie's clock follows its soundtrack when it has one */
    dcfmv_set_audio_clock_mode(v->fmv, dcfmv_audio_channels(v->fmv) > 0);
    if (dcfmv_audio_channels(v->fmv) > 0) {
        aud_music_settle();   /* a music stop still under way must let go of its stream first */
        if (dcfmv_audio_init(v->fmv) < 0) dcfmv_set_audio_clock_mode(v->fmv, 0);
        else {
            v->audio_ready = true;
            dcfmv_set_audio_volume(v->fmv, 204);   /* the core's voice bus level (0.8) */
            /* the menu stops its music before the intro; the briefing's voice-only video plays over it, as on the PC */
        }
    }
    /* the first frames synchronously, the rest by the worker */
    int nf = v->fmv->num_total_frames;
    for (int f = 0; f < 4 && f < nf; f++) {
        int buf = dcfmv_total_to_unique(v->fmv, f) % DCFMV_NUM_BUFFERS;
        atomic_store(&v->fmv->buf_state[buf], DCFMV_BUF_LOADING);
        if (dcfmv_load_frame(v->fmv, f, buf) != 0) atomic_store(&v->fmv->buf_state[buf], DCFMV_BUF_EMPTY);
    }
    for (int f = 0; f < DCFMV_NUM_BUFFERS && f < nf; f++) dcfmv_schedule_frame_preload(v->fmv, f);
    v->worker = thd_create(0, worker, v);
    return v;
}

static void start(Video *v)
{
    if (v->started) return;
    v->started = true;
    dcfmv_reanchor_clock_to_current_frame(v->fmv);
    if (v->audio_ready) { dcfmv_audio_start_stream(v->fmv); dcfmv_set_audio_muted(v->fmv, 0); }
}

Video *video_open(Ren *r, uint32_t id)
{
    (void)r;
    char path[64]; snprintf(path, sizeof path, "/cd/video/%08lX.dcmv", (unsigned long)id);
    Video *v = open_path(path);
    /* The briefing source is 768x312. Conversion reduces every movie to a
     * 320x240 texture, but the room screen is sized from the source video. */
    if (v && id == 0x2FE798C3u) { v->w = 768; v->h = 312; }
    return v;
}

void video_preload_file(const char *path, real fps) { (void)path; (void)fps; }

Video *video_open_file(Ren *r, const char *path, float fps)
{
    (void)r; (void)fps;   /* the rate is in the DCMV header */
    if (!path) return NULL;
    char p[256]; snprintf(p, sizeof p, "%s", path);
    char *dot = strrchr(p, '.'); if (dot) *dot = 0;
    strncat(p, ".dcmv", sizeof p - strlen(p) - 1);
    return open_path(p);
}

bool video_update(Video *v, float dt)
{
    (void)dt;   /* the movie runs on its own clock (its soundtrack's, or the timer) */
    if (!v || v->finished) return false;
    start(v);
    dcfmv_tick(v->fmv);
    if (dcfmv_frame_index(v->fmv) >= v->fmv->num_total_frames - 1) { v->finished = true; return false; }
    return true;
}

static void draw(Video *v, float x, float y, float w, float h)
{
    if (!v || !rdc_in_frame()) return;
    dcfmv_upload_current_video(v->fmv);
    if (v->fmv->last_unique_frame_drawn < 0) return;   /* nothing decoded yet */
    float X0, Y0, X1, Y1;
    extern void rdc_xform(float x, float y, float *X, float *Y);
    rdc_xform(x, y, &X0, &Y0); rdc_xform(x + w, y + h, &X1, &Y1);
    RdcVert q[4] = { { X0, Y0, 1, v->u0, v->v0, 0xffffffffu, 0 }, { X1, Y0, 1, v->u1, v->v0, 0xffffffffu, 0 },
                     { X1, Y1, 1, v->u1, v->v1, 0xffffffffu, 0 }, { X0, Y1, 1, v->u0, v->v1, 0xffffffffu, 0 } };
    rdc_header(&v->hdr);
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
    v->quit = true;
    if (v->worker) thd_join(v->worker, NULL);
    dcfmv_set_audio_muted(v->fmv, 1);
    dcfmv_close(v->fmv);
    dcfmv_destroy(v->fmv);
    rdc_forget_header();
    rdc_vram_free(v->tex, v->tex_bytes);
    free(v);
}
