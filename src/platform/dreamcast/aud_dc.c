/* platform/aud.h on the AICA.
 *
 * Every sample is baked into snd.pck by tools/dc/build_disc.py: 4-bit Yamaha ADPCM made by KOS's wav2adpcm (the packs'
 * sfx - their own ADPCM flavour restarts its predictor every 0x7ff8 bytes, which the AICA can't - and our wav files,
 * mono, at their own rate), padded to whole 32-byte units, one block each ("SMPL", u32 rate, samples, bytes, 16 bytes
 * 0, the data): one read from the disc, no header parsing. Pack sfx are under their id, our files under asset_key.
 * A sample that fits the AICA's 16-bit channel length (65534 samples) goes to sound RAM once and is fired with
 * snd_sfx; a longer one (dialog lines, the power-attack speeches) or a loop stays in main RAM (its block) and plays
 * through one of two KOS streams, which also carry a volume we can change while it plays.
 * Loops are decoded once to PCM16 so repeating them cannot carry an ADPCM predictor into a fresh encoded sample.
 * Music is ADX, decoded by music_adx.c; only our music worker owns its KOS stream. */
#include "../aud.h"
#include "../plat.h"
#include "../../pack.h"
#include "../../assets.h"
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include "music_adx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define AICA_MAX_LEN 65534
#define LONG_CACHE (768 * 1024)   /* main RAM kept for long samples (least recently used ones go first) */
#define NSTREAM 2
#define STREAM_BUF 16384          /* ADPCM bytes per stream buffer (0.74 s at 44.1 kHz) */
#define VOICE_BUF SND_STREAM_BUFFER_MAX_ADPCM /* PCM turbo ring: ~0.74 s at 22.05 kHz; ADPCM still fits 16-bit positions */
#define MUSIC_BUF SND_STREAM_BUFFER_MAX

struct AudSample {
    uint32_t key;                 /* its snd.pck block: a pack sfx id or asset_key */
    bool missing;
    int rate, samples;
    sfxhnd_t sfx;                 /* short: in sound RAM */
    const uint8_t *adpcm; size_t bytes; /* long: its block in main RAM (NULL until needed); bytes: whole 32-byte units */
    int16_t *loop_pcm;            /* a loop decoded with its own initial predictor, retained in main RAM */
    uint32_t last_use;
    bool kept;                    /* long, loaded for good (aud_keep) */
};
#define MAX_SAMPLES 200
static AudSample samples[MAX_SAMPLES]; static int nsamples;
static uint32_t use_clock;
static size_t long_bytes;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool prepare_loop(AudSample *s);

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
    if (s->rate <= 0 || s->samples <= 0 || !s->bytes || (s->bytes & 31) ||
        s->bytes > e->size - 32 || s->bytes != (size_t)s->samples / 2 || (s->samples & 1)) {
        printf("snd: sample %08lX invalid\n", (unsigned long)s->key); s->missing = true; return NULL;
    }
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
    if (s && loop && !prepare_loop(s)) return;
    if (!s || (s->sfx && !loop) || s->kept) return;   /* short one-shots are in sound RAM already; loops stream from main RAM */
    if (s->adpcm || load_long(s)) { s->kept = true; long_bytes -= s->bytes; }   /* outside the LRU budget */
}
void aud_unkeep(AudSample *s) { (void)s; }   /* scene release is currently Saturn-specific */
AudSample *aud_sample_file(const char *path) { return path ? find(asset_key(path)) : NULL; }

/* ---- streams for long samples and loops ---- */
static uint8_t silence[STREAM_BUF] __attribute__((aligned(32)));
/* KOS starts DMA after the callback returns. Alternate scratch buffers so a
 * later callback cannot overwrite the source of the preceding DMA. */
static uint8_t pad[NSTREAM][2][VOICE_BUF] __attribute__((aligned(32)));
static struct {
    snd_stream_hnd_t h; AudSample *smp; size_t pos; bool loop, active, draining; uint64_t end_ms; unsigned gen, pad_index; int vol;
} strm[NSTREAM];

static bool prepare_loop(AudSample *s)
{
    if (s->loop_pcm) return true;
    if (!s->adpcm && !load_long(s)) return false;
    int16_t *pcm = memalign(32, s->bytes * 4);
    if (!pcm) return false;
    /* KOS wav2adpcm's public-domain YMZ decoder: low nibble first, with
     * history 0 and step 127 at the beginning of each encoded sample. */
    static const int step_table[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int history = 0, step = 127;
    for (int i = 0; i < s->samples; i++) {
        unsigned nibble = (s->adpcm[i / 2] >> ((i & 1) * 4)) & 15;
        int delta = (int)(nibble & 7);
        int diff = ((1 + delta * 2) * step) >> 3;
        if (diff > 32767) diff = 32767;
        history = history * 254 / 256;
        history += (nibble & 8) ? -diff : diff;
        if (history > 32767) history = 32767;
        if (history < -32768) history = -32768;
        pcm[i] = (int16_t)history;
        step = (step_table[delta] * step) >> 8;
        if (step < 127) step = 127;
        if (step > 24576) step = 24576;
    }
    s->loop_pcm = pcm;
    return true;
}

static void *stream_cb(snd_stream_hnd_t hnd, int req, int *got)
{
    for (int i = 0; i < NSTREAM; i++) {
        if (strm[i].h != hnd) continue;
        size_t want = (size_t)req < VOICE_BUF ? (size_t)req : VOICE_BUF;
        uint8_t *out = pad[i][strm[i].pad_index++ & 1];
        memset(out, strm[i].loop ? 0 : 0x80, want);
        *got = (int)want;
        if (!strm[i].active || strm[i].draining || !strm[i].smp) return out;
        AudSample *s = strm[i].smp;
        const uint8_t *data = strm[i].loop ? (const uint8_t *)s->loop_pcm : s->adpcm;
        size_t bytes = strm[i].loop ? (size_t)s->samples * 2 : s->bytes;
        if (!data || !bytes) return out;
        size_t done = 0;
        while (done < want) {
            size_t n = bytes - strm[i].pos;
            if (n > want - done) n = want - done;
            memcpy(out + done, data + strm[i].pos, n);
            done += n; strm[i].pos += n;
            if (strm[i].pos != bytes) continue;
            if (strm[i].loop) strm[i].pos = 0;
            else {   /* the tail and silence fill the complete KOS request */
                strm[i].draining = true;
                strm[i].end_ms = timer_ms_gettime64() + (uint64_t)VOICE_BUF * 2 * 1000 / (uint64_t)s->rate + 50;
                break;
            }
        }
        return out;
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
    if (loop ? !prepare_loop(s) : (!s->adpcm && !load_long(s))) return -1;
    int best = -1;
    for (int i = 0; i < NSTREAM; i++) if (!strm[i].active) { best = i; break; }
    if (best < 0) {   /* both busy: take the one that is not looping, else the first */
        for (int i = 0; i < NSTREAM; i++) if (!strm[i].loop) { best = i; break; }
        if (best < 0) best = 0;
        snd_stream_stop(strm[best].h);
    }
    strm[best].smp = s; strm[best].pos = 0; strm[best].loop = loop; strm[best].draining = false;
    strm[best].active = true; strm[best].gen++; strm[best].vol = vol_of(gain);
    /* Queue the mono start and its initial volume together. Turbo starts at
     * gain 0; KOS's default volume of 255 must not leak out before its fade. */
    snd_stream_queue_enable(strm[best].h);
    if (loop) snd_stream_start(strm[best].h, s->rate, 0);
    else snd_stream_start_adpcm(strm[best].h, s->rate, 0);
    snd_stream_volume(strm[best].h, strm[best].vol);
    snd_stream_queue_go(strm[best].h);
    snd_stream_queue_disable(strm[best].h);
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

/* ---- music: one worker owns a persistent PCM16 KOS stream ----
 * Track switches, decoding, polling, gain, and EOF all happen on this worker.
 * There is no detached libADX driver to reuse a handle or destroy other streams.
 * music_adx.c retains small tracks and reads larger tracks ahead on a separate
 * thread; only that reader touches its file descriptor. */
static snd_stream_hnd_t music_h = SND_STREAM_INVALID;
static DcAdx *music_adx;
static bool music_on, music_paused, music_draining;   /* worker-owned */
static uint64_t music_end_ms, music_pause_ms;
static int music_vol = -1;
static uint8_t music_pcm[2][MUSIC_BUF] __attribute__((aligned(32)));
static unsigned music_pcm_index;
static struct { uint32_t id; bool loop, paused; unsigned seq; int volume; } want = { .volume = 255 };
static unsigned done_seq;
static bool music_idle = true;
static mutex_t music_mx = MUTEX_INITIALIZER;
static semaphore_t music_sem;
static kthread_t *music_thd;
static bool music_quit;                              /* under music_mx */

static void *music_cb(snd_stream_hnd_t hnd, int req, int *got)
{
    (void)hnd;
    uint8_t *out = music_pcm[music_pcm_index++ & 1];
    size_t bytes = (size_t)req < MUSIC_BUF ? (size_t)req : MUSIC_BUF;
    memset(out, 0, bytes);
    *got = (int)bytes;
    if (!music_adx || music_paused || music_draining) return out;
    unsigned channels = dc_adx_channels(music_adx);
    dc_adx_read(music_adx, (int16_t *)out, bytes / (channels * 2));
    if (dc_adx_done(music_adx)) {
        /* The last partial PCM block is followed by zeros, never old samples.
         * Give the whole AICA ring time to play before stopping the channel. */
        music_draining = true;
        music_end_ms = timer_ms_gettime64() + (uint64_t)MUSIC_BUF * 1000 /
                       (2 * dc_adx_rate(music_adx)) + 50;
    }
    return out;
}

static void music_stop_now(void)
{
    if (music_on) snd_stream_stop(music_h);
    music_on = music_paused = music_draining = false;
    music_vol = -1;
    dc_adx_close(music_adx); music_adx = NULL;
}

static void *music_worker(void *arg)
{
    (void)arg;
    for (;;) {
        mutex_lock(&music_mx);
        uint32_t id = want.id;
        bool loop = want.loop, paused = want.paused, quit = music_quit;
        unsigned seq = want.seq;
        int volume = want.volume;
        mutex_unlock(&music_mx);
        if (quit) break;
        if (seq != done_seq) {
            music_stop_now();
            if (id) {
                char path[64]; snprintf(path, sizeof path, "/cd/music/%08lX.adx", (unsigned long)id);
                music_adx = dc_adx_open(path, loop);
                /* A stop or another track can arrive while disc I/O is in
                 * flight. Discard that obsolete open before starting it. */
                mutex_lock(&music_mx);
                bool stale = want.seq != seq;
                volume = want.volume; paused = want.paused;
                mutex_unlock(&music_mx);
                if (stale) { music_stop_now(); continue; }
                if (music_adx) {
                    music_paused = paused;
                    music_pause_ms = timer_ms_gettime64();
                    snd_stream_start(music_h, dc_adx_rate(music_adx), dc_adx_channels(music_adx) == 2);
                    music_on = true;
                    music_vol = paused ? 0 : volume;
                    snd_stream_volume(music_h, music_vol);
                } else printf("music: %s failed\n", path);
            }
            done_seq = seq;
        }
        if (music_on) {
            uint64_t now = timer_ms_gettime64();
            if (paused != music_paused) {
                if (paused) music_pause_ms = now;
                else if (music_draining) music_end_ms += now - music_pause_ms;
                music_paused = paused;
            }
            int v = paused ? 0 : volume;
            if (v != music_vol) { snd_stream_volume(music_h, v); music_vol = v; }
            if (music_draining && !paused && now >= music_end_ms) music_stop_now();
            else snd_stream_poll(music_h);
        }
        mutex_lock(&music_mx);
        music_idle = done_seq == want.seq && (!music_on ||
                     (music_paused == want.paused && music_vol == (want.paused ? 0 : want.volume)));
        bool idle = music_idle;
        mutex_unlock(&music_mx);
        if (!idle) continue;
        if (music_on) sem_wait_timed(&music_sem, 10);
        else sem_wait(&music_sem);
    }
    music_stop_now();
    return NULL;
}

static void music_post(void (*edit)(uint32_t, bool), uint32_t id, bool loop)
{
    mutex_lock(&music_mx);
    edit(id, loop); music_idle = false;
    mutex_unlock(&music_mx);
    sem_signal(&music_sem);
}
static void want_track(uint32_t id, bool loop) { want.id = id; want.loop = loop; want.paused = false; want.seq++; }
static void want_pause(uint32_t id, bool pause) { (void)id; want.paused = pause; }

/* Wait for a requested stop before video disc reads or audio shutdown. */
void aud_music_settle(void)
{
    for (;;) {
        mutex_lock(&music_mx); bool idle = music_idle; mutex_unlock(&music_mx);
        if (idle) return;
        thd_sleep(2);
    }
}

bool aud_music_play(uint32_t id, bool loop) { music_post(want_track, id, loop); return true; }
void aud_music_stop(void) { music_post(want_track, 0, false); }
void aud_music_gain(float g)
{
    int v = vol_of(g);
    mutex_lock(&music_mx);
    bool changed = want.volume != v;
    want.volume = v;
    if (changed) music_idle = false;
    mutex_unlock(&music_mx);
    if (changed) sem_signal(&music_sem);
}
void aud_music_pause(bool pause) { music_post(want_pause, 0, pause); }

/* ---- video soundtrack: whole ADPCM body preloaded to RAM (a .vsnd sidecar next to the .zamv, the same "SMPL" +
 * rate/samples/bytes header + KOS wav2adpcm body as a snd.pck sample block). ZAMV5 carries no audio of its own
 * (unlike the old DCMV files), so video_dcmv.c drives this stream instead, and paces its decode against
 * aud_video_audio_ms() the way the old code paced against dcfmv's own audio clock.
 *
 * This used to fs_read() a few KB straight off the disc from inside the AICA poll callback, once per buffer refill.
 * That let the video's own zamv_chunk_reader (video_dcmv.c, also fs_read on the same /cd filesystem, same thread)
 * and this callback fight over the GD-ROM's one read head: two files, .zamv and .vsnd, growing apart on disc as
 * playback goes on, so every few hundred ms the drive had to seek away from the video bitstream to fetch a few KB
 * of audio and back again. On real hardware (GDEMU) that is exactly "random drop out": a seek stalls whichever
 * fs_read lost the race, which starves either the video decoder (a stuck frame) or the AICA stream (an audible
 * gap), and since video_update() paces frames off this stream's clock a stalled audio read stalls the
 * picture too. A video's sidecar tops out in the low single-digit MB (largest current one 1.3 MB) - small enough to
 * pull in with one read at open time, the same "one read from the disc, no header parsing" rule this file already
 * applies to every other sample (see the file's own top comment) - after which nothing here touches the disc again
 * until the next video opens.
 *
 * The video's clock (aud_video_audio_ms) is the time since the stream started, held back to what has been handed to
 * the AICA. It used to be the bytes handed over, but KOS asks for them half a buffer (0.37 s) at a time and a whole
 * buffer ahead of what plays: the picture ran 0.74 s early and moved in 0.37 s lurches - nine frames in a burst, then
 * a freeze. */
static uint8_t *vaud_data;
static size_t vaud_bytes_total;
static size_t vaud_pos;
static uint32_t vaud_rate;
static uint64_t vaud_bytes_played;   /* handed to the AICA (the stream buffer runs ahead of what is heard) */
static uint64_t vaud_start_us;
static snd_stream_hnd_t vaud_h = SND_STREAM_INVALID;

void aud_video_audio_close(void);

static void *vaud_cb(snd_stream_hnd_t hnd, int req, int *got)
{
    if (hnd != vaud_h || !vaud_data) { *got = req; return silence; }
    size_t want = (size_t)req < sizeof silence ? (size_t)req : sizeof silence;
    size_t n = vaud_bytes_total - vaud_pos; if (n > want) n = want;
    void *p = vaud_data + vaud_pos;
    vaud_pos += n;
    if (n < want) {   /* pad the tail in place: playback never wraps back over the sample's own bytes */
        memset(vaud_data + vaud_pos, 0x80, want - n);   /* ADPCM has no zero code: +step/8, -step/8 holds the level */
    }
    vaud_bytes_played += want;
    *got = (int)want;
    return p;
}

bool aud_video_audio_open(const char *path)
{
    aud_video_audio_close();
    file_t fd = fs_open(path, O_RDONLY);
    if (fd < 0) return false;
    uint8_t hdr[32];
    if (fs_read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr || memcmp(hdr, "SMPL", 4)) { fs_close(fd); return false; }
    uint32_t rate = rd32(hdr + 4), bytes = rd32(hdr + 12);
    /* pad one full silence buffer past the end: vaud_cb's tail request can ask for up to sizeof(silence) bytes
     * starting at vaud_pos == bytes, and that read must land in owned memory even though it is then overwritten. */
    uint8_t *buf = (uint8_t *)memalign(32, (size_t)bytes + sizeof silence);
    if (!buf || fs_read(fd, buf, bytes) != (ssize_t)bytes) { free(buf); fs_close(fd); return false; }
    fs_close(fd);
    vaud_data = buf; vaud_bytes_total = bytes; vaud_pos = 0;
    vaud_rate = rate; vaud_bytes_played = 0;
    if (vaud_h == SND_STREAM_INVALID) vaud_h = snd_stream_alloc(vaud_cb, STREAM_BUF);
    if (vaud_h == SND_STREAM_INVALID) { free(buf); vaud_data = NULL; return false; }
    snd_stream_start_adpcm(vaud_h, vaud_rate, 0);
    vaud_start_us = timer_us_gettime64();
    snd_stream_volume(vaud_h, 204);   /* the core's voice bus level (0.8), same as the old dcfmv path used */
    return true;
}
void aud_video_audio_close(void)
{
    if (vaud_h != SND_STREAM_INVALID) snd_stream_stop(vaud_h);
    free(vaud_data); vaud_data = NULL;
}
uint32_t aud_video_audio_ms(void)
{
    if (!vaud_rate) return 0;
    uint64_t ms = (timer_us_gettime64() - vaud_start_us) / 1000;
    uint64_t fed_ms = vaud_bytes_played * 2000 / vaud_rate;   /* 2 samples per ADPCM byte */
    return (uint32_t)(ms < fed_ms ? ms : fed_ms);
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
    if (vaud_data) snd_stream_poll(vaud_h);
}

bool aud_init(void)
{
    if (snd_stream_init() < 0) return false;
    memset(silence, 0x80, sizeof silence);   /* +step/8, -step/8, ...: holds the level (ADPCM has no zero code) */
    for (int i = 0; i < NSTREAM; i++) {
        strm[i].h = snd_stream_alloc(stream_cb, VOICE_BUF);
        if (strm[i].h == SND_STREAM_INVALID) {
            printf("snd: no stream %d\n", i); snd_stream_shutdown(); return false;
        }
    }
    music_h = snd_stream_alloc(music_cb, MUSIC_BUF);
    if (music_h == SND_STREAM_INVALID) { snd_stream_shutdown(); return false; }
    sem_init(&music_sem, 0);
    music_thd = thd_create(false, music_worker, NULL);
    if (!music_thd) { sem_destroy(&music_sem); snd_stream_shutdown(); return false; }
    return true;
}

void aud_shutdown(void)
{
    aud_music_stop(); aud_music_settle();
    mutex_lock(&music_mx); music_quit = true; mutex_unlock(&music_mx);
    sem_signal(&music_sem); thd_join(music_thd, NULL); music_thd = NULL;
    sem_destroy(&music_sem);
    for (int i = 0; i < NSTREAM; i++) if (strm[i].h != SND_STREAM_INVALID) { snd_stream_stop(strm[i].h); snd_stream_destroy(strm[i].h); }
    aud_video_audio_close();
    if (vaud_h != SND_STREAM_INVALID) { snd_stream_destroy(vaud_h); vaud_h = SND_STREAM_INVALID; }
    snd_stream_destroy(music_h); music_h = SND_STREAM_INVALID;
    for (int i = 0; i < nsamples; i++) free(samples[i].loop_pcm);
    snd_sfx_unload_all();
    snd_stream_shutdown();
}
