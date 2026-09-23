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
    bool owns_music, owns_sfx;   /* which audio the video started (stopped with it) */
    /* our own clips (video_open_file): a raw MPEG-4 part 2 stream split into packets by libavcodec's parser */
    uint8_t *file; size_t file_size, file_pos;
    AVCodecParserContext *parser; bool parser_flushed, decoder_flushed;
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
        if (!memcmp(a, "MUPS", 4)) v->owns_music = music_play_blob(a, audio_size, false);
        else if (!memcmp(a, "RIFF", 4)) { sfx_play_blob(a, audio_size); v->owns_sfx = true; }
    }
    return v;
}

static void show_frame(Video *v)
{
    if (!v->sws) v->sws = sws_getContext(v->fr->width, v->fr->height, v->fr->format, v->w, v->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    void *pix; int pitch;
    if (SDL_LockTexture(v->tex, NULL, &pix, &pitch)) {
        uint8_t *dst[1] = { pix }; int ls[1] = { pitch };
        sws_scale(v->sws, (const uint8_t *const *)v->fr->data, v->fr->linesize, 0, v->fr->height, dst, ls);
        SDL_UnlockTexture(v->tex);
    }
    v->have_frame = true;
}

/* the next picture of a video_open_file stream; false at its end (nframes is then the count shown) */
static bool decode_next_stream(Video *v)
{
    for (;;) {
        if (avcodec_receive_frame(v->ctx, v->fr) == 0) { show_frame(v); v->cur++; return true; }
        if (v->decoder_flushed) { v->nframes = v->cur; return false; }
        uint8_t *out = NULL; int out_size = 0;
        if (v->file_pos < v->file_size) {
            int used = av_parser_parse2(v->parser, v->ctx, &out, &out_size, v->file + v->file_pos, (int)(v->file_size - v->file_pos), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            v->file_pos += used > 0 ? (size_t)used : v->file_size - v->file_pos;
        } else if (!v->parser_flushed) {   /* the parser holds the last picture until it is told the stream ended */
            av_parser_parse2(v->parser, v->ctx, &out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            v->parser_flushed = true;
        } else { avcodec_send_packet(v->ctx, NULL); v->decoder_flushed = true; continue; }
        if (out_size > 0 && av_new_packet(v->pkt, out_size) == 0) {
            memcpy(v->pkt->data, out, (size_t)out_size);
            avcodec_send_packet(v->ctx, v->pkt);
            av_packet_unref(v->pkt);
        }
    }
}

Video *video_open_file(SDL_Renderer *r, const char *path, float fps)
{
    SDL_IOStream *io = path ? SDL_IOFromFile(path, "rb") : NULL;
    if (!io) return NULL;
    Sint64 size = SDL_GetIOSize(io);
    Video *v = calloc(1, sizeof *v);
    v->file = size > 0 ? av_mallocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE) : NULL;
    if (!v->file || SDL_ReadIO(io, v->file, (size_t)size) != (size_t)size) { SDL_CloseIO(io); av_free(v->file); free(v); return NULL; }
    SDL_CloseIO(io);
    v->file_size = (size_t)size; v->fps = fps; v->nframes = 1 << 30;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    v->ctx = c ? avcodec_alloc_context3(c) : NULL;
    v->parser = av_parser_init(AV_CODEC_ID_MPEG4);
    if (!v->ctx || !v->parser || avcodec_open2(v->ctx, c, NULL) < 0) { video_close(v); return NULL; }
    v->pkt = av_packet_alloc(); v->fr = av_frame_alloc();
    v->w = 320; v->h = 240;   /* the clips are made at the pack videos' size (tools/build_power_assets.py); sws scales to it anyway */
    v->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, v->w, v->h);
    if (!v->tex) { video_close(v); return NULL; }
    SDL_SetTextureScaleMode(v->tex, SDL_SCALEMODE_LINEAR);
    return v;
}

static bool decode_next(Video *v)
{
    if (v->parser) return decode_next_stream(v);
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
        if (avcodec_receive_frame(v->ctx, v->fr) == 0) { show_frame(v); return true; }
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
    /* FUN_0042d0a0(video layer) -> FUN_00411480: the video's own audio stops with it; a RIFF-voiced video (the
     * briefing) leaves the menu music alone */
    if (v->owns_music) music_stop();
    if (v->owns_sfx) sfx_stop_blob();
    if (v->sws) sws_freeContext(v->sws);
    if (v->parser) av_parser_close(v->parser);
    av_free(v->file);
    av_frame_free(&v->fr); av_packet_free(&v->pkt); avcodec_free_context(&v->ctx);
    if (v->tex) SDL_DestroyTexture(v->tex);
    free(v);
}
