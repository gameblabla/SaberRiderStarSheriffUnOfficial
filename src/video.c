#include "video.h"
#include "pack.h"
#include "audio.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct Video {
    const uint8_t *data; uint32_t size;
    int nframes, cur; const uint32_t *sizes; const uint8_t *p;
    AVCodecContext *ctx; AVPacket *pkt; AVFrame *fr; struct SwsContext *sws;
    SDL_Texture *tex; int w, h;
    float t, fps;
    bool finished, have_frame;
};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

Video *video_open(SDL_Renderer *r, uint32_t id)
{
    const PackEntry *e = packs_find_type(id, RES_VIDEO);
    if (!e || e->size < 64) return NULL;
    Video *v = calloc(1, sizeof *v);
    v->data = e->data; v->size = e->size;
    v->w = rd32(e->data + 4) & 0xffff; v->h = rd32(e->data + 4) >> 16;
    uint32_t audio_off = rd32(e->data + 8), audio_size = rd32(e->data + 12);
    v->nframes = rd32(e->data + 16);
    v->sizes = (const uint32_t *)(e->data + 32);
    v->p = e->data + 32 + v->nframes * 4;
    v->fps = 25.0f;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    v->ctx = avcodec_alloc_context3(c);
    if (avcodec_open2(v->ctx, c, NULL) < 0) { free(v); return NULL; }
    v->pkt = av_packet_alloc(); v->fr = av_frame_alloc();
    v->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, v->w, v->h);
    SDL_SetTextureScaleMode(v->tex, SDL_SCALEMODE_LINEAR);
    /* audio */
    if (audio_off + audio_size <= e->size && audio_size > 12) {
        const uint8_t *a = e->data + audio_off;
        if (!memcmp(a, "MUPS", 4)) music_play_blob(a, audio_size, false);
        else if (!memcmp(a, "RIFF", 4)) sfx_play_blob(a, audio_size);
    }
    return v;
}

static bool decode_next(Video *v)
{
    while (v->cur < v->nframes) {
        uint32_t sz = v->sizes[v->cur];
        uint8_t *buf = av_malloc(sz + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(buf, v->p, sz);
        for (uint32_t k = 0; k < 16 && k < sz; k++) buf[k] = (uint8_t)(buf[k] - (0x35 + k));
        memset(buf + sz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_packet_from_data(v->pkt, buf, (int)sz);
        v->p += sz; v->cur++;
        avcodec_send_packet(v->ctx, v->pkt);
        av_packet_unref(v->pkt);
        if (avcodec_receive_frame(v->ctx, v->fr) == 0) {
            if (!v->sws) v->sws = sws_getContext(v->fr->width, v->fr->height, v->fr->format, v->w, v->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
            void *pix; int pitch;
            if (SDL_LockTexture(v->tex, NULL, &pix, &pitch)) {
                uint8_t *dst[1] = { pix }; int ls[1] = { pitch };
                sws_scale(v->sws, (const uint8_t *const *)v->fr->data, v->fr->linesize, 0, v->fr->height, dst, ls);
                SDL_UnlockTexture(v->tex);
            }
            v->have_frame = true;
            return true;
        }
    }
    return false;
}

bool video_update(Video *v, float dt)
{
    if (!v || v->finished) return false;
    v->t += dt;
    int want = (int)(v->t * v->fps);
    while (v->cur <= want && v->cur < v->nframes) {
        if (!decode_next(v)) break;
    }
    if (v->cur >= v->nframes && v->t * v->fps > v->nframes + 1) { v->finished = true; return false; }
    return true;
}

void video_draw(Video *v, SDL_Renderer *r, int sw, int sh)
{
    if (!v || !v->have_frame) return;
    float scale = (float)sw / v->w; if (v->h * scale > sh) scale = (float)sh / v->h;
    SDL_FRect dst = { (sw - v->w * scale) * 0.5f, (sh - v->h * scale) * 0.5f, v->w * scale, v->h * scale };
    SDL_RenderTexture(r, v->tex, NULL, &dst);
}

void video_draw_rect(Video *v, SDL_Renderer *r, float x, float y, float w, float h)
{
    if (!v || !v->have_frame) return;
    SDL_FRect dst = { x, y, w, h };
    SDL_RenderTexture(r, v->tex, NULL, &dst);
}
void video_size(const Video *v, int *w, int *h) { *w = v ? v->w : 0; *h = v ? v->h : 0; }

void video_close(Video *v)
{
    if (!v) return;
    music_stop();
    if (v->sws) sws_freeContext(v->sws);
    av_frame_free(&v->fr); av_packet_free(&v->pkt); avcodec_free_context(&v->ctx);
    if (v->tex) SDL_DestroyTexture(v->tex);
    free(v);
}
