/* platform/aud.h on the AICA.
 *
 * Every sample on the disc is 4-bit Yamaha ADPCM made by KOS's wav2adpcm (tools/dc/build_disc.py converts the packs'
 * sfx - their own ADPCM flavour restarts its predictor every 0x7ff8 bytes, which the AICA can't - and our wav files,
 * mono, at their own rate). A sample that fits the AICA's 16-bit channel length (65534 samples) is loaded into sound
 * RAM once and fired with snd_sfx; a longer one (dialog lines, the power-attack speeches) or a loop stays in main
 * RAM and plays through one of two ADPCM streams, which also carry a volume we can change while it plays.
 * Music is the packs' tracks as ADX files, streamed from the disc by libADX in its own thread. */
#include "../aud.h"
#include "../plat.h"
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <adx/adx.h>
#include <adx/snddrv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define AICA_MAX_LEN 65534
#define LONG_CACHE (768 * 1024)   /* main RAM kept for long samples (least recently used ones go first) */
#define NSTREAM 2
#define STREAM_BUF 16384          /* ADPCM bytes per stream buffer (0.74 s at 44.1 kHz) */

struct AudSample {
    uint32_t id; char path[128];
    bool loaded, missing;
    int rate, samples;
    sfxhnd_t sfx;                 /* short: in sound RAM */
    uint8_t *adpcm; size_t bytes; /* long: in main RAM (NULL until needed) */
    uint32_t last_use;
};
#define MAX_SAMPLES 200
static AudSample samples[MAX_SAMPLES]; static int nsamples;
static uint32_t use_clock;
static size_t long_bytes;

/* ---- wav files ---- */
typedef struct { int rate, ch, bits; uint32_t data_off, data_len; } WavInfo;
static bool wav_info(const char *path, WavInfo *wi)
{
    FILE *f = fopen(path, "rb"); if (!f) return false;
    uint8_t h[12]; bool ok = false;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint32_t pos = 12; uint8_t c[8];
        while (fseek(f, pos, SEEK_SET) == 0 && fread(c, 1, 8, f) == 8) {
            uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            if (!memcmp(c, "fmt ", 4)) {
                uint8_t fm[16]; if (fread(fm, 1, 16, f) != 16) break;
                wi->ch = fm[2] | fm[3] << 8; wi->rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24; wi->bits = fm[14] | fm[15] << 8;
            } else if (!memcmp(c, "data", 4)) { wi->data_off = pos + 8; wi->data_len = len; ok = true; break; }
            pos += 8 + len + (len & 1);
        }
    }
    fclose(f);
    return ok;
}

static bool load_long(AudSample *s)
{
    WavInfo wi;
    if (!wav_info(s->path, &wi)) return false;
    while (long_bytes + wi.data_len > LONG_CACHE) {   /* make room: drop the least recently used long sample not playing */
        AudSample *old = NULL;
        for (int i = 0; i < nsamples; i++) if (samples[i].adpcm && (!old || samples[i].last_use < old->last_use)) old = &samples[i];
        extern bool snd_dc_sample_busy(const AudSample *s);
        if (!old || old == s || snd_dc_sample_busy(old)) break;
        free(old->adpcm); old->adpcm = NULL; long_bytes -= old->bytes;
    }
    uint8_t *d = memalign(32, (wi.data_len + 31) & ~31u);
    FILE *f = d ? fopen(s->path, "rb") : NULL;
    bool ok = f && fseek(f, wi.data_off, SEEK_SET) == 0 && fread(d, 1, wi.data_len, f) == wi.data_len;
    if (f) fclose(f);
    if (!ok) { free(d); return false; }
    s->adpcm = d; s->bytes = wi.data_len; long_bytes += wi.data_len;
    return true;
}

static AudSample *load(AudSample *s)
{
    WavInfo wi;
    s->loaded = true;
    if (!wav_info(s->path, &wi) || wi.bits != 4 || wi.ch != 1) {
        printf("snd: %s missing or not mono ADPCM\n", s->path); s->missing = true; return NULL;
    }
    s->rate = wi.rate; s->samples = (int)(wi.data_len * 2);
    if (s->samples <= AICA_MAX_LEN) {
        s->sfx = snd_sfx_load(s->path);
        if (s->sfx == SFXHND_INVALID) { printf("snd: %s: no sound RAM\n", s->path); s->missing = true; return NULL; }
    }
    return s;
}

static AudSample *find(uint32_t id, const char *path)
{
    for (int i = 0; i < nsamples; i++)
        if (path ? !strcmp(samples[i].path, path) : (samples[i].id == id && samples[i].path[0] == '/' && !strncmp(samples[i].path, "/cd/sfx/", 8)))
            return samples[i].missing ? NULL : &samples[i];
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++]; memset(s, 0, sizeof *s);
    s->id = id;
    if (path) snprintf(s->path, sizeof s->path, "%s", path);
    else snprintf(s->path, sizeof s->path, "/cd/sfx/%08lX.wav", (unsigned long)id);
    return load(s);
}

AudSample *aud_sample_pack(uint32_t id) { return find(id, NULL); }
AudSample *aud_sample_file(const char *path) { return path ? find(0, path) : NULL; }

/* ---- streams for long samples and loops ---- */
static uint8_t silence[STREAM_BUF] __attribute__((aligned(32)));
static struct {
    snd_stream_hnd_t h; AudSample *smp; size_t pos; bool loop, active, draining; uint64_t end_ms; unsigned gen; int vol;
} strm[NSTREAM];

static void *stream_cb(snd_stream_hnd_t hnd, int req, int *got)
{
    for (int i = 0; i < NSTREAM; i++) {
        if (strm[i].h != hnd) continue;
        if (!strm[i].active || strm[i].draining || !strm[i].smp || !strm[i].smp->adpcm) { *got = req; return silence; }
        AudSample *s = strm[i].smp;
        if (strm[i].pos >= s->bytes) {
            if (strm[i].loop) strm[i].pos = 0;
            else {   /* ran out: let what is queued play, then stop (aud_update) */
                strm[i].draining = true;
                strm[i].end_ms = timer_ms_gettime64() + (uint64_t)STREAM_BUF * 2 * 1000 / (uint64_t)s->rate + 50;
                *got = req; return silence;
            }
        }
        size_t n = s->bytes - strm[i].pos; if (n > (size_t)req) n = (size_t)req;
        void *p = s->adpcm + strm[i].pos; strm[i].pos += n;
        *got = (int)n;
        return p;
    }
    *got = 0; return NULL;
}

bool snd_dc_sample_busy(const AudSample *s)
{
    for (int i = 0; i < NSTREAM; i++) if (strm[i].active && strm[i].smp == s) return true;
    return false;
}

static int vol_of(float g) { int v = (int)(g * 255.0f + 0.5f); return v < 0 ? 0 : v > 255 ? 255 : v; }

/* voice handles: short sfx = the AICA channel (0..63); streams = 64 + index, with a generation in the upper bits */
int aud_play(AudSample *s, float gain, bool loop)
{
    if (!s) return -1;
    s->last_use = ++use_clock;
    if (s->sfx && !loop) {
        sfx_play_data_t pd = { .chn = -1, .idx = s->sfx, .vol = vol_of(gain), .pan = 128, .loop = 0, .freq = s->rate };
        int ch = snd_sfx_play_ex(&pd);
        return ch >= 0 ? ch : -1;
    }
    if (!s->adpcm && !load_long(s)) return -1;
    int best = -1;
    for (int i = 0; i < NSTREAM; i++) if (!strm[i].active) { best = i; break; }
    if (best < 0) {   /* both busy: take the one that is not looping, else the first */
        for (int i = 0; i < NSTREAM; i++) if (!strm[i].loop) { best = i; break; }
        if (best < 0) best = 0;
        snd_stream_stop(strm[best].h);
    }
    strm[best].smp = s; strm[best].pos = 0; strm[best].loop = loop; strm[best].draining = false;
    strm[best].active = true; strm[best].gen++; strm[best].vol = vol_of(gain);
    snd_stream_start_adpcm(strm[best].h, s->rate, 0);
    snd_stream_volume(strm[best].h, strm[best].vol);
    return 64 + best + (int)(strm[best].gen << 8);
}

static int stream_of(int voice) { if (voice < 64) return -1; int i = (voice - 64) & 0xff; return i < NSTREAM && (int)(64 + i + (strm[i].gen << 8)) == voice && strm[i].active ? i : -1; }

void aud_set_gain(int voice, float gain)
{
    int i = stream_of(voice); if (i < 0) return;
    int v = vol_of(gain); if (v != strm[i].vol) { strm[i].vol = v; snd_stream_volume(strm[i].h, v); }
}
void aud_stop(int voice)
{
    if (voice < 0) return;
    if (voice < 64) { snd_sfx_stop(voice); return; }
    int i = stream_of(voice); if (i < 0) return;
    snd_stream_stop(strm[i].h); strm[i].active = false; strm[i].smp = NULL;
}
bool aud_playing(int voice) { return voice >= 64 ? stream_of(voice) >= 0 : voice >= 0; }

/* ---- music: ADX from the disc ---- */
extern snd_stream_hnd_t shnd;   /* libADX's stream (its snddrv.c) */
static bool music_on, music_paused; static float music_gain = 1.0f; static int music_vol = -1;
static void apply_music_vol(void)
{
    /* the handle is valid once libADX's driver thread streams (a fresh track resets its volume) */
    if (!music_on || snddrv.drv_status != SNDDRV_STATUS_STREAMING) { music_vol = -1; return; }
    int v = vol_of(music_gain);
    if (v != music_vol) { snd_stream_volume(shnd, v); music_vol = v; }
}
/* libADX's driver thread ends with snd_stream_shutdown(), which would destroy every stream (our two, a video's) and
 * hand their handles to the next track; the link wraps it (Makefile.dc: --wrap) so it only tells us the thread is done.
 * The stream system lives from aud_init to aud_shutdown. */
static volatile unsigned adx_drv_exits;
void __real_snd_stream_shutdown(void);
void __wrap_snd_stream_shutdown(void) { adx_drv_exits++; }

/* wait (up to ms) for cond, letting libADX's threads run */
#define WAIT_FOR(cond, ms) do { uint64_t t_end_ = timer_ms_gettime64() + (ms); \
                                while (!(cond) && timer_ms_gettime64() < t_end_) thd_sleep(1); } while (0)

bool aud_music_play(uint32_t id, bool loop)
{
    aud_music_stop();
    char path[64]; snprintf(path, sizeof path, "/cd/music/%08lX.adx", (unsigned long)id);
    if (!adx_dec(path, loop ? 1 : 0)) { printf("music: %s failed\n", path); return false; }
    music_on = true; music_paused = false; music_vol = -1;
    return true;
}
void aud_music_stop(void)
{
    if (!music_on) return;
    music_on = false;
    /* a stop right after a start (the briefing's music, then its video) raced libADX: its driver thread, still starting
     * its stream, set STREAMING over the stop's DONE and adx_stop spun forever. So let the driver come up first, and
     * after the stop wait for it to release its stream before anyone allocates another */
    if (music_paused) { adx_resume(); music_paused = false; }
    WAIT_FOR(snddrv.dec_status == SNDDEC_STATUS_STREAMING && snddrv.drv_status == SNDDRV_STATUS_STREAMING, 1000);
    unsigned exits = adx_drv_exits;
    bool driver = snddrv.drv_status != SNDDRV_STATUS_NULL;
    adx_stop();
    if (driver) WAIT_FOR(adx_drv_exits != exits, 500);
}
void aud_music_gain(float g) { music_gain = g; }
void aud_music_pause(bool pause)
{
    if (!music_on || pause == music_paused) return;
    if (pause) adx_pause(); else adx_resume();
    music_paused = pause;
}

/* ---- lifetime ---- */
void aud_update(void)
{
    uint64_t now = timer_ms_gettime64();
    for (int i = 0; i < NSTREAM; i++) {
        if (!strm[i].active) continue;
        if (strm[i].draining && now >= strm[i].end_ms) { snd_stream_stop(strm[i].h); strm[i].active = false; strm[i].smp = NULL; continue; }
        snd_stream_poll(strm[i].h);
    }
    apply_music_vol();
}

bool aud_init(void)
{
    snd_stream_init();
    memset(silence, 0x80, sizeof silence);   /* +step/8, -step/8, ...: holds the level (ADPCM has no zero code) */
    for (int i = 0; i < NSTREAM; i++) {
        strm[i].h = snd_stream_alloc(stream_cb, STREAM_BUF);
        if (strm[i].h == SND_STREAM_INVALID) { printf("snd: no stream %d\n", i); return false; }
    }
    return true;
}

void aud_shutdown(void)
{
    aud_music_stop();
    for (int i = 0; i < NSTREAM; i++) if (strm[i].h != SND_STREAM_INVALID) { snd_stream_stop(strm[i].h); snd_stream_destroy(strm[i].h); }
    snd_sfx_unload_all();
    __real_snd_stream_shutdown();
}
