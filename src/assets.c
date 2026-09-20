#include "assets.h"
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return false; fclose(f); return true; }

const char *asset_path(const char *name)
{
    static char buf[1024];
    const char *env = SDL_getenv("SABER_ASSETS");
    if (env) { snprintf(buf, sizeof buf, "%s/%s", env, name); if (exists(buf)) return buf; }
    snprintf(buf, sizeof buf, "assets/%s", name); if (exists(buf)) return buf;
    const char *base = SDL_GetBasePath();
    if (base) {
        snprintf(buf, sizeof buf, "%sassets/%s", base, name); if (exists(buf)) return buf;
        snprintf(buf, sizeof buf, "%s../assets/%s", base, name); if (exists(buf)) return buf;
    }
    return NULL;
}

uint8_t *file_read(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n > 0 ? (size_t)n + AV_INPUT_BUFFER_PADDING_SIZE : 64);
    if (n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (n > 0) memset(d + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *size = n > 0 ? (size_t)n : 0;
    return d;
}

uint32_t *png_load_rgba(const char *path, int *w, int *h)
{
    size_t size; uint8_t *data = file_read(path, &size);
    if (!data) return NULL;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = c ? avcodec_alloc_context3(c) : NULL;
    uint32_t *px = NULL;
    if (ctx && avcodec_open2(ctx, c, NULL) >= 0) {
        AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
        pkt->data = data; pkt->size = (int)size;
        if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, fr) >= 0) {
            *w = fr->width; *h = fr->height;
            px = malloc((size_t)fr->width * fr->height * 4);
            struct SwsContext *sws = sws_getContext(fr->width, fr->height, fr->format, fr->width, fr->height, AV_PIX_FMT_RGBA, 0, NULL, NULL, NULL);
            uint8_t *dst[1] = { (uint8_t *)px }; int stride[1] = { fr->width * 4 };
            sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, stride);
            sws_freeContext(sws);
        } else fprintf(stderr, "%s: png decode failed\n", path);
        av_packet_free(&pkt); av_frame_free(&fr);
    }
    if (ctx) avcodec_free_context(&ctx);
    free(data);
    return px;
}
