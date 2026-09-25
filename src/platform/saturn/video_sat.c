/* Sega Saturn Cinepak player: Sega FILM/CPK with ADX audio, using the
 * libyaul_cinepak player in third_party/libyaul_cinepak.
 *
 * The movie is decoded directly into the renderer's VDP1 video surface while
 * VDP1 is idle.  The CPU-only FILM sample table stays with decode_work_t in
 * low work RAM; the 96 KiB compressed-sample ring also lives in low work RAM.
 * The callback I/O path is CPU/SH-2-DMAC driven, so it does not require the
 * SCU-DMA-visible high-RAM sample buffer used by the original direct-CD path.
 * Movie bytes come through Saber Rider's existing sequential CDFS streamer,
 * avoiding a second CD-block owner while keeping libyaul's Cinepak/ADX decode.
 */
#include "../../video.h"
#include "../render.h"
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "film_lib.h"
#include "film_buff.h"
#include "film_snd.h"
#include "pcmsys.h"

void rsat_video_draw(const RFRect *dst);   /* render_sat.c */
const cdfs_filelist_entry_t *cd_sat_entry(const char *name); /* cd_sat.c */

#define SAMPLE_BUFFER_BYTES (96 * 1024)

int film_loop_handler(void) { return 1; }

struct Video {
    const cdfs_filelist_entry_t *entry;
    FILE *stream;
    int w, h;                 /* physical CPK/decode dimensions */
    int logical_w, logical_h; /* source-space dimensions exposed to game UI */
    decode_work_t *work;
    decode_param_t params;
    uint8_t *sample_mem;
    bool done, surface;
};

static bool driver_ok;
static bool driver_tried;
static Video *prepared_video;
static char prepared_name[16];
/* One persistent 68000 sound driver is shared by the game mixer and movie
 * audio. Load it during boot so neither gameplay SFX/music nor an in-game
 * cut-in pays the SCSP clear/driver-start cost. */
bool rsat_sound_driver_init(void)
{
    if (driver_ok) return true;
    if (driver_tried) return false;
    driver_tried = true;

    FILE *f = fopen("SNDDRV.BIN", "rb");
    if (!f) { printf("video: SNDDRV.BIN missing\n"); return false; }
    uint8_t *buf = malloc(32 * 1024);
    size_t n = buf ? fread(buf, 1, 32 * 1024, f) : 0;
    fclose(f);
    cd_sat_stream_stop();
    if (!buf || !n) { free(buf); printf("video: could not read SNDDRV.BIN\n"); return false; }

    pcmsys_load_driver(buf, (uint32_t)n);
    pcmStreamInitialize();
    free(buf);
    driver_ok = true;
    return true;
}

/* Pay the one-time SCSP movie-driver setup cost during boot instead of on the
 * first in-game cut-in.  On real Saturn timing this includes clearing sound
 * RAM and starting the 68000 driver, which is far too expensive for a power
 * button press. */
void rsat_video_preload(void)
{
    (void)rsat_sound_driver_init();
}

static void stop_sound(void)
{
    if (!driver_ok) return;
    film_audio_reset();
}

/* Called by render_sat.c after VDP1 has finished the previous list and before
 * it submits the next one.  Never decode more than the frame that is due: once
 * isDisplayReady is acknowledged we break, so that frame stays in VDP1 memory
 * for the command list that immediately follows this hook. */
static void hook(void *ud, volatile uint16_t *px, int pitch)
{
    Video *v = ud;
    if (v->done || !v->work) return;

    decode_param_t *p = &v->params;
    p->vramBuffAddr = p->vramWritePos = (uint32_t *)px;
    p->vramBufferWidth = (int16_t)pitch;
    p->vramBufferHeight = (int16_t)v->h;
    p->vramDelta = 0;
    p->vramBuffSize = pitch * v->h * 2;

    for (int pass = 0; pass < 4 && v->work->play_status != END &&
         v->work->play_status != ERROR; pass++) {
        cpk_task(v->work);
        /* The refill path owns any transfer it starts.  Do not wait on a DMA
         * channel here: with the game I/O backend most ticks start no DMA,
         * and an unconditional wait can deadlock movie playback. */
        if (v->work->isDisplayReady) {
            cpk_display_finished(v->work);
            break;
        }
    }
    if (v->work->play_status == END || v->work->play_status == ERROR)
        v->done = true;
}

static uint32_t movie_read(void *user, void *dst, uint32_t len)
{
    return (uint32_t)fread(dst, 1, len, (FILE *)user);
}

static uint32_t movie_available(void *user)
{
    size_t n = cd_sat_available((FILE *)user);
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

static void movie_name(const char *path, char name[16])
{
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    int n = 0;
    while (*b && *b != '.' && n < 8) name[n++] = (char)toupper((unsigned char)*b++);
    strcpy(name + n, ".CPK");
}

static bool activate_video(Video *v)
{
    if (v->surface) return true;
    film_buff_io_set(v->stream, movie_read, movie_available);
    v->surface = rsat_video_open(v->w, v->h, hook, v);
    if (!v->surface) film_buff_io_clear();
    return v->surface;
}

static Video *open_name(const char *name, bool activate)
{
    if (!rsat_sound_driver_init()) return NULL;
    const cdfs_filelist_entry_t *entry = cd_sat_entry(name);
    if (!entry) { printf("video: %s missing\n", name); return NULL; }
    FILE *stream = fopen(name, "rb");
    if (!stream) { printf("video: %s open failed\n", name); return NULL; }

    /* All movie bookkeeping is CPU-owned; do not spend the last few KiB of
     * high work RAM merely to create a cut-in while a stage is resident. */
    Video *v = lw_malloc(sizeof *v);
    if (!v) { fclose(stream); return NULL; }
    memset(v, 0, sizeof *v);
    v->work = lw_memalign(4, sizeof *v->work);
    v->sample_mem = lw_memalign(32, SAMPLE_BUFFER_BYTES);
    if (!v->work || !v->sample_mem) {
        printf("video: %s: no RAM for Cinepak stream\n", name);
        free(v->sample_mem); free(v->work); fclose(stream); free(v); return NULL;
    }

    memset(v->work, 0, sizeof *v->work);
    memset(&v->params, 0, sizeof v->params);
    v->entry = entry;
    v->stream = stream;
    v->params.sampleBuffAddr = (uint32_t *)v->sample_mem;
    v->params.sampleBuffSize = SAMPLE_BUFFER_BYTES;
    v->params.decodeColorDepth = COLOR_DEPTH_15;
    v->params.audioEnable = true;
    v->params.pcmVolume = 7;
    v->params.pcmChannels = 1;
    v->params.pcmPan = 0;
    v->params.pcmTransferMode = PCM_XFER_SH2_DMA;
    v->params.audioBufferAddr = (int32_t)(uintptr_t)(SNDRAM + 0x18000);
    v->params.audioBufferSize = 16384;
    v->work->decodeParams = &v->params;

    /* Keep the game's existing CDFS streamer in charge of the drive.  The
     * libyaul decoder still owns FILM/Cinepak/ADX parsing and buffering, but
     * its byte source is the game's FILE stream. */
    film_buff_io_set(stream, movie_read, movie_available);
    init_film_start((cdfs_filelist_entry_t *)entry, v->work, 0, 0);
    if (v->work->play_status == ERROR || memcmp(v->work->filmHeader.film_str, "FILM", 4) ||
        memcmp(v->work->filmHeader.fdsc.fdsc_str, "FDSC", 4) ||
        memcmp(v->work->filmHeader.fdsc.fourcc, "cvid", 4) ||
        v->work->filmHeader.fdsc.sound_codec != FDSC_CODEC_ADX) {
        printf("video: %s: unsupported FILM/CPK\n", name);
        film_buff_io_clear();
        stop_sound();
        fclose(v->stream);
        free(v->sample_mem); free(v->work); free(v); return NULL;
    }

    v->w = v->work->filmHeader.fdsc.width;
    v->h = v->work->filmHeader.fdsc.height;
    v->logical_w = v->w;
    v->logical_h = v->h;
    if (activate) {
        if (!activate_video(v)) {
            printf("video: %s: no room for %dx%d surface\n", name, v->w, v->h);
            stop_sound();
            fclose(v->stream);
            free(v->sample_mem); free(v->work); free(v); return NULL;
        }
    } else {
        /* Parsing/prefill is complete; no decoder runs until the clip is
         * actually requested.  Release the singleton callback binding while
         * the game continues loading the stage. */
        film_buff_io_clear();
    }

    printf("video: %s %dx%d, %d samples, ADX %u Hz%s\n", name, v->w, v->h,
           (int)v->work->filmHeader.stab.total_entries,
           (unsigned)(v->work->filmHeader.fdsc.sample_rate >> 16),
           activate ? "" : " (preloaded)");
    return v;
}

Video *video_open(Ren *r, uint32_t id)
{
    (void)r;
    char name[16]; snprintf(name, sizeof name, "%08X.CPK", (unsigned)id);
    Video *v = open_name(name, true);
    /* The PC/Dreamcast briefing source is 768x312, but Saturn stores a
       256x104 CPK and lets VDP1 scale it.  menu.c intentionally draws this
       source at 1/3 size, so expose the original logical dimensions while
       retaining the compact physical decode surface. */
    if (v && id == 0x2FE798C3u) {
        v->logical_w = 768;
        v->logical_h = 312;
    }
    return v;
}

void video_preload_file(const char *path, real fps)
{
    (void)fps;
    char name[16] = {0};
    if (path) movie_name(path, name);
    if (prepared_video && path && !strcmp(name, prepared_name)) return;
    if (prepared_video) {
        Video *old = prepared_video; prepared_video = NULL; prepared_name[0] = 0;
        video_close(old);
    }
    if (!path) return;
    prepared_video = open_name(name, false);
    if (prepared_video) snprintf(prepared_name, sizeof prepared_name, "%s", name);
}

Video *video_open_file(Ren *r, const char *path, real fps)
{
    (void)r; (void)fps;
    if (!path) return NULL;
    char name[16]; movie_name(path, name);
    if (prepared_video && !strcmp(name, prepared_name)) {
        Video *v = prepared_video;
        prepared_video = NULL; prepared_name[0] = 0;
        if (!activate_video(v)) { video_close(v); return NULL; }
        return v;
    }
    if (prepared_video) video_preload_file(NULL, 0);
    return open_name(name, true);
}

bool video_update(Video *v, real dt) { (void)dt; return v && !v->done; }

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
    int w = v->w, h = v->h;
    if (w > sw || h > sh) {
        if (w * sh > h * sw) { h = h * sw / w; w = sw; }
        else { w = w * sh / h; h = sh; }
    }
    video_draw_rect(v, r, r_int((sw - w) / 2), r_int((sh - h) / 2), r_int(w), r_int(h));
}

void video_size(const Video *v, int *w, int *h)
{
    if (w) *w = v ? v->logical_w : 0;
    if (h) *h = v ? v->logical_h : 0;
}

void video_close(Video *v)
{
    if (!v) return;
    if (v->surface) rsat_video_close();
    film_buff_io_clear();
    stop_sound();
    if (v->stream) fclose(v->stream);
    cd_sat_stream_stop();
    free(v->sample_mem);
    free(v->work);
    free(v);
}
