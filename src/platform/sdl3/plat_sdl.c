/* platform/plat.h on SDL3 (+ libavcodec for PNG decoding) */
#include "../plat.h"
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *plat_getenv(const char *name) { return SDL_getenv(name); }
uint64_t plat_ticks_ms(void) { return SDL_GetTicks(); }
const char *plat_base_path(void) { return SDL_GetBasePath(); }
/* SaberRider/data under the current folder (the repo), else data/ next to the executable (a release) */
const char *plat_default_data_dir(void)
{
    static char buf[1024];
    const char *base = SDL_GetBasePath();
    if (SDL_GetPathInfo("SaberRider/data/pack.pck", NULL) || !base) return "SaberRider/data";
    snprintf(buf, sizeof buf, "%sdata", base);
    return buf;
}
int plat_default_ratio(void) { return 0; }   /* RATIO_WIDE */
int plat_screen_modes(void) { return 4; }
int plat_wide_width(int screen) { (void)screen; return 426; }
bool plat_screen_43_only(int screen) { (void)screen; return false; }
void plat_screen_label(int screen, char *buf, size_t n)
{
    if (screen == 0) snprintf(buf, n, "FULL %ux%u", 852, 480); else snprintf(buf, n, "WINDOWED x%u", screen + 1);
}

void plat_apply_screen(Ren *r, int sw, int sh, int ratio, int screen)
{
    SDL_Renderer *ren = (SDL_Renderer *)r;
    SDL_SetRenderLogicalPresentation(ren, sw, sh, ratio == 1 ? SDL_LOGICAL_PRESENTATION_STRETCH : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_Window *win = SDL_GetRenderWindow(ren);
    if (win && screen >= 0) {
        SDL_SetWindowFullscreen(win, screen == 0);
        if (screen > 0) SDL_SetWindowSize(win, 426 * (screen + 1), 240 * (screen + 1));
    }
}

uint32_t *plat_image_load_rgba(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return NULL; }
    fclose(f);
    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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
        }
        av_packet_free(&pkt); av_frame_free(&fr);
    }
    if (ctx) avcodec_free_context(&ctx);
    free(data);
    return px;
}

bool plat_screenshot(Ren *r, const char *path)
{
    SDL_Surface *sf = SDL_RenderReadPixels((SDL_Renderer *)r, NULL);
    if (!sf) return false;
    bool ok = SDL_SaveBMP(sf, path);
    SDL_DestroySurface(sf);
    return ok;
}
