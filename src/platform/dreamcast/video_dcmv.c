/* video.h on the Dreamcast: videos converted to ZAMV5 (third_party/zamv, from KOS's zamv5-sh4zam-r8 example) by
 * tools/dc/build_disc.py, decoded with SH4ZAM acceleration straight into the PVR's hardware YUV converter
 * (PVR_TA_YUV_CONV) - no VQ texture, no runtime LZ unpack, one Store Queue gather per macroblock row. The decoder
 * itself has no notion of audio: a video's soundtrack is a separate .vsnd sidecar (mono AICA ADPCM, the same shape
 * as a snd.pck sample block) streamed by aud_dc.c's own dedicated channel, and playback paces its frames against
 * that stream's played-byte count, falling back to a wall-clock accumulator for silent clips (the power-attack
 * animations). Pack videos are /cd/video/<ID>.zamv(+.vsnd), our clips the same path as the clip with those
 * extensions. */
#include "../../video.h"
#include "pvr_internal.h"
#include <zamv.h>
#include <zamv_dc_helpers.h>
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* aud_dc.c: the video's own dedicated AICA ADPCM channel (see its top-of-file comment) */
bool     aud_video_audio_open(const char *path);
void     aud_video_audio_close(void);
uint64_t aud_video_audio_played_bytes(void);
uint32_t aud_video_audio_rate(void);
void     aud_music_settle(void);   /* the music worker has carried out every request (a stream about to take its own) */

struct Video {
    file_t fd;
    zamv_chunk_reader_t *chunk;
    zamv_decoder_t *dec;
    zamv_file_header_t hdr;
    zamv_dc_row_uploader_t up;
    /* Double-buffered: the ZAMV5 decoder writes straight into PVR texture memory (no separate host-side
     * decode buffer to DMA later), and video_update() runs during app_update(), before rdc_frame_begin()'s
     * pvr_wait_ready() - so a single texture would race the PowerVR's tile render pass, which may still be
     * sampling the previous frame from that same VRAM when the next frame's SQ upload starts (visible as
     * sporadic top-of-frame corruption). tex[1-cur] is always at least one full frame past its last draw
     * before decode_one() writes into it again. */
    pvr_ptr_t tex[2]; size_t tex_bytes; int tw, th, cur;
    uint32_t fmt;
    pvr_poly_hdr_t phdr __attribute__((aligned(32)));
    float u0, v0, u1, v1;
    int w, h;                 /* display size (may differ from the encoded/padded frame size) */
    int frame_index;
    bool finished, has_audio, started;
    float clock_ms;           /* the wall-clock fallback for videos with no soundtrack */
};

static int pot(int n) { int p = 8; while (p < n) p <<= 1; return p; }

static int fs_read_cb(void *user, void *dst, size_t want, size_t *got)
{
    file_t fd = (file_t)(intptr_t)user;
    ssize_t n = fs_read(fd, dst, want);
    if (n < 0) return -1;
    if (got) *got = (size_t)n;
    return 0;
}

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

static Video *open_zamv(const char *path, const char *audio_path)
{
    file_t fd = fs_open(path, O_RDONLY);
    if (fd < 0) { printf("video: %s missing\n", path); return NULL; }
    Video *v = memalign(32, sizeof *v);
    if (!v) { fs_close(fd); return NULL; }
    memset(v, 0, sizeof *v);
    v->fd = fd;
    v->chunk = zamv_chunk_reader_create(fs_read_cb, (void *)(intptr_t)fd, 64 * 1024);
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
    if (!setup_texture(v)) goto fail;
    zamv_decoder_set_row_callback(v->dec, zamv_dc_upload_row_direct_cb, &v->up);

    if (audio_path) {
        aud_music_settle();   /* a music stop still under way must let go of its stream first */
        v->has_audio = aud_video_audio_open(audio_path);
    }
    return v;
fail:
    if (v->dec) zamv_decoder_destroy(v->dec);
    if (v->chunk) zamv_chunk_reader_destroy(v->chunk);
    fs_close(fd);
    free(v);
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

static bool decode_one(Video *v)
{
    int next = 1 - v->cur;   /* not read by the GPU since at least the frame before last: see the Video comment */
    if (zamv_dc_configure_yuv420(v->tex[next], v->hdr.width, v->hdr.height) < 0) return false;
    if (zamv_dc_row_uploader_begin(&v->up) < 0) return false;
    int dr = zamv_decoder_decode_cb(v->dec, zamv_chunk_reader_read, v->chunk, NULL);
    zamv_dc_row_uploader_end(&v->up);
    if (dr <= 0) { v->finished = true; return false; }
    v->cur = next;
    rdc_compile(&v->phdr, v->tex[v->cur], v->fmt, v->tw, v->th, R_BLEND_NONE, true, false, false);
    v->frame_index++;
    return true;
}

bool video_update(Video *v, float dt)
{
    if (!v || v->finished) return false;
    if (!v->started) { v->started = true; v->clock_ms = 0; }
    float frame_ms = 1000.0f * (float)v->hdr.fps_den / (float)(v->hdr.fps_num ? v->hdr.fps_num : 1);
    float target_ms;
    if (v->has_audio) {
        uint64_t rate = aud_video_audio_rate();
        target_ms = rate ? (float)(aud_video_audio_played_bytes() * 2000ull / rate) : 0;   /* 2 samples/ADPCM byte */
    } else {
        v->clock_ms += dt * 1000.0f;
        target_ms = v->clock_ms;
    }
    if (target_ms >= (float)(v->frame_index + 1) * frame_ms) decode_one(v);
    return !v->finished;
}

static void draw(Video *v, float x, float y, float w, float h)
{
    if (!v || !rdc_in_frame() || !v->frame_index) return;
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
    if (v->has_audio) aud_video_audio_close();
    if (v->dec) zamv_decoder_destroy(v->dec);
    if (v->chunk) zamv_chunk_reader_destroy(v->chunk);
    fs_close(v->fd);
    rdc_forget_header();
    rdc_vram_free(v->tex[0], v->tex_bytes);
    rdc_vram_free(v->tex[1], v->tex_bytes);
    free(v);
}
