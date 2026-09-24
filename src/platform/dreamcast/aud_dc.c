/* platform/aud.h on the AICA.
 *
 * Every sample is baked into snd.pck by tools/dc/build_disc.py: 4-bit Yamaha ADPCM made by KOS's wav2adpcm (the packs'
 * sfx - their own ADPCM flavour restarts its predictor every 0x7ff8 bytes, which the AICA can't - and our wav files,
 * mono, at their own rate), padded to whole 32-byte units, one block each ("SMPL", u32 rate, samples, bytes, 16 bytes
 * 0, the data): one read from the disc, no header parsing. Pack sfx are under their id, our files under asset_key.
 * A sample that fits the AICA's 16-bit channel length (65534 samples) goes to sound RAM once and is fired with
 * snd_sfx; a longer one (dialog lines, the power-attack speeches) or a loop stays in main RAM (its block) and plays
 * through one of two ADPCM streams, which also carry a volume we can change while it plays.
 * Music is the packs' tracks as ADX files, streamed from the disc by libADX in its own thread. */
#include "../aud.h"
#include "../plat.h"
#include "../../pack.h"
#include "../../assets.h"
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
    uint32_t key;                 /* its snd.pck block: a pack sfx id or asset_key */
    bool missing;
    int rate, samples;
    sfxhnd_t sfx;                 /* short: in sound RAM */
    const uint8_t *adpcm; size_t bytes; /* long: its block in main RAM (NULL until needed); bytes: whole 32-byte units */
    uint32_t last_use;
    bool kept;                    /* long, loaded for good (aud_keep) */
};
#define MAX_SAMPLES 200
static AudSample samples[MAX_SAMPLES]; static int nsamples;
static uint32_t use_clock;
static size_t long_bytes;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* a long sample's block stays in main RAM (s->adpcm points into it); the least recently used ones not playing go when
 * the cache would pass LONG_CACHE */
static void make_room(const AudSample *s)
{
    while (long_bytes + s->bytes > LONG_CACHE) {
        AudSample *old = NULL;
        for (int i = 0; i < nsamples; i++) if (samples[i].adpcm && !samples[i].kept && &samples[i] != s && (!old || samples[i].last_use < old->last_use)) old = &samples[i];
        extern bool snd_dc_sample_busy(const AudSample *s);
        if (!old || snd_dc_sample_busy(old)) break;
        packs_release_type(old->key, RES_SAMPLE); old->adpcm = NULL; long_bytes -= old->bytes;
    }
}
static bool load_long(AudSample *s)
{
    if (s->adpcm) return true;
    make_room(s);
    const PackEntry *e = packs_find_type(s->key, RES_SAMPLE);
    if (!e || e->size < 32 + s->bytes) return false;
    s->adpcm = e->data + 32; long_bytes += s->bytes;
    return true;
}

static AudSample *load(AudSample *s)
{
    const PackEntry *e = packs_find_type(s->key, RES_SAMPLE);
    if (!e || e->size < 32 || memcmp(e->data, "SMPL", 4)) { printf("snd: sample %08lX missing\n", (unsigned long)s->key); s->missing = true; return NULL; }
    s->rate = (int)rd32(e->data + 4); s->samples = (int)rd32(e->data + 8); s->bytes = rd32(e->data + 12);
    if (s->samples <= AICA_MAX_LEN) {   /* to sound RAM; the block goes */
        s->sfx = snd_sfx_load_raw_buf((char *)e->data + 32, s->bytes, (uint32_t)s->rate, 4, 1);
        packs_release_type(s->key, RES_SAMPLE);
        if (s->sfx == SFXHND_INVALID) { printf("snd: %08lX: no sound RAM\n", (unsigned long)s->key); s->missing = true; return NULL; }
    } else if (e->size >= 32 + s->bytes) {   /* long: the block just read is what the stream plays */
        make_room(s);
        s->adpcm = e->data + 32; long_bytes += s->bytes;
    }
    return s;
}

static AudSample *find(uint32_t key)
{
    for (int i = 0; i < nsamples; i++) if (samples[i].key == key) return samples[i].missing ? NULL : &samples[i];
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++]; memset(s, 0, sizeof *s);
    s->key = key;
    return load(s);
}

AudSample *aud_sample_pack(uint32_t id) { return find(id); }
void aud_prefetch(AudSample *s) { if (s && !s->sfx && !s->adpcm) { s->last_use = ++use_clock; load_long(s); } }
void aud_keep(AudSample *s, bool loop)
{
    if (!s || (s->sfx && !loop) || s->kept) return;   /* short one-shots are in sound RAM already; loops stream from main RAM */
    if (s->adpcm || load_long(s)) { s->kept = true; long_bytes -= s->bytes; }   /* outside the LRU budget */
}
AudSample *aud_sample_file(const char *path) { return path ? find(asset_key(path)) : NULL; }

/* ---- streams for long samples and loops ---- */
static uint8_t silence[STREAM_BUF] __attribute__((aligned(32)));
#ifdef AUD_UNPADDED_SAMPLES
static uint8_t pad[NSTREAM][STREAM_BUF] __attribute__((aligned(32)));   /* a sample's padded last chunk */
#endif
static struct {
    snd_stream_hnd_t h; AudSample *smp; size_t pos; bool loop, active, draining; uint64_t end_ms; unsigned gen; int vol;
} strm[NSTREAM];

static void *stream_cb(snd_stream_hnd_t hnd, int req, int *got)
{
    for (int i = 0; i < NSTREAM; i++) {
        if (strm[i].h != hnd) continue;
        if (!strm[i].active || strm[i].draining || !strm[i].smp || !strm[i].smp->adpcm || !strm[i].smp->bytes) { *got = req; return silence; }
        AudSample *s = strm[i].smp;
        if (strm[i].pos >= s->bytes) {
            if (strm[i].loop) strm[i].pos = 0;
            else {   /* ran out: let what is queued play, then stop (aud_update) */
                strm[i].draining = true;
                strm[i].end_ms = timer_ms_gettime64() + (uint64_t)STREAM_BUF * 2 * 1000 / (uint64_t)s->rate + 50;
                *got = req; return silence;
            }
        }
        size_t n = s->bytes - strm[i].pos;
        if (n >= (size_t)req) { const void *p = s->adpcm + strm[i].pos; strm[i].pos += (size_t)req; *got = req; return (void *)p; }
#ifndef AUD_UNPADDED_SAMPLES
        /* the tail: KOS writes each chunk at the running offset in sound RAM, so a chunk that is not whole 32-byte units
         * would misalign every later DMA of the stream (g2_dma refuses them and it goes silent for good). The samples are
         * baked as whole 32-byte units, so the tail goes as it is; the next request loops or drains */
        { const void *p = s->adpcm + strm[i].pos; strm[i].pos = s->bytes; *got = (int)n; return (void *)p; }
#else
        /* unpadded samples: always fill the request, a loop carrying on from its start, a one-shot with silence */
        size_t want = (size_t)req < STREAM_BUF ? (size_t)req : STREAM_BUF, done = 0;
        while (done < want) {
            size_t k = s->bytes - strm[i].pos; if (k > want - done) k = want - done;
            memcpy(pad[i] + done, s->adpcm + strm[i].pos, k); done += k; strm[i].pos += k;
            if (strm[i].pos < s->bytes) continue;
            if (!strm[i].loop) { memset(pad[i] + done, 0, want - done); done = want; }
            else strm[i].pos = 0;
        }
        *got = (int)want;
        return pad[i];
#endif
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
