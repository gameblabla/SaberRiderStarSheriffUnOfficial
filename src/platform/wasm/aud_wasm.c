/* platform/aud.h for the WASM port: a software mixer writing into a ring buffer the page's AudioWorklet reads.
 *
 * The core keeps all the sound logic (audio.c: the sfx table and its random variants, the retrigger windows, the
 * delays, the hero overrides, the voice slot, the loop fades, the music fade and duck), and this file is only the
 * mixer it drives, as aud_sdl.c is for the PC: N voices of PCM, resampled to the device rate, summed with the
 * core's bus gains, and the limiter the SDL build runs on the device's post-mix run here instead.
 *
 * Music is the packs' MUPS, which is Ogg Vorbis with the pages and the codec id renamed (mups_to_ogg restores
 * it), decoded by stb_vorbis (third_party/wasm): there is no libvorbis to link against in a freestanding
 * wasm32 module and no emscripten here, and stb_vorbis is a single public-domain file. It is opened over the
 * whole restored Ogg in memory, which is what a music track is small enough to be (a couple of MB), and read
 * a frame's worth at a time. Looping is stb_vorbis_seek back to the start.
 *
 * The output is interleaved S16 stereo at WASM_AUDIO_RATE. The page copies out whatever is ready each frame and
 * posts it to the worklet, the same arrangement the reference WASM port uses; the ring holds half a second, so a
 * late frame does not click. */
/* stb_vorbis first: it defines an R macro of its own (a playback mode flag), and real.h - which comes in with
 * aud.h below - defines R(c), a constant. Whichever is seen second would warn, so the decoder goes first and its
 * macros are put back the way they were afterwards. */
#define STB_VORBIS_NO_STDIO
/* stb_vorbis's temp_alloc is `alloca(size)` unless the caller gave it an allocator; alloca.h is pulled in first so
 * that name is the builtin (see the note in it) rather than a call to nothing. */
#include <alloca.h>
#include "../../../third_party/wasm/stb_vorbis.c"
#undef R
#undef L

#include "../aud.h"
#include "../common/sfx_decode.h"
#include "../common/mups.h"
#include "../../pack.h"
#include "wasm_internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ the output ring */
static int16_t ring[WASM_AUDIO_RING_FRAMES * 2];
static int ring_read, ring_write, ring_count;   /* in frames, the write side is the module's */
static float master = 1.0f;

int16_t *audio_ring(void) { return ring; }
/* the oldest unread frame (the page copies from here, not from the ring's base: the unread run starts at the
 * read index and may wrap) and how many frames are contiguous from it */
int16_t *audio_read_ptr(void) { return ring + (size_t)ring_read * 2; }
int audio_contig(void)
{
    if (ring_count <= 0) return 0;
    int room = WASM_AUDIO_RING_FRAMES - ring_read;
    return ring_count < room ? ring_count : room;
}
int audio_pending(void) { return ring_count; }
void audio_set_master(float v) { master = v < 0 ? 0 : v > 1 ? 1 : v; }

int audio_take(int frames)
{
    if (frames > ring_count) frames = ring_count;
    ring_read = (ring_read + frames) % WASM_AUDIO_RING_FRAMES;
    ring_count -= frames;
    return frames;
}
static void ring_push(int16_t l, int16_t r)
{
    if (ring_count >= WASM_AUDIO_RING_FRAMES) return;   /* the page is behind: drop, as the worklet would */
    ring[ring_write * 2] = l;
    ring[ring_write * 2 + 1] = r;
    ring_write = (ring_write + 1) % WASM_AUDIO_RING_FRAMES;
    ring_count++;
}

/* ------------------------------------------------------------------ the limiter
 * The core leaves each bus some headroom (audio.c's GAIN_SFX 0.6, GAIN_VOICE 0.8, GAIN_MUSIC 0.9) and the sum
 * still clips, so the same peak limiter the SDL build runs on the device's post-mix runs here: an instant attack
 * and an exponential release, -1 dBFS. */
#define LIMIT_CEIL 0.89f
static float lim_env, lim_release;

static inline void limit_and_store(float l, float r, int16_t *out)
{
    /* the SDL build's release (LIMIT_RELEASE_S 0.12 s): exp(-1 / (0.12 * 44100)) per sample. A shorter constant
     * pumps: the envelope would collapse between cycles and the gain ride every sample, pinning everything
     * at the ceiling. */
    if (lim_release == 0.0f) lim_release = 0.999811f;
    float peak = l < 0 ? -l : l;
    float pr = r < 0 ? -r : r;
    if (pr > peak) peak = pr;
    lim_env = peak > lim_env ? peak : lim_env * lim_release;
    float g = lim_env > LIMIT_CEIL ? LIMIT_CEIL / lim_env : 1.0f;
    l *= g * master;
    r *= g * master;
    if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
    if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
    out[0] = (int16_t)(l * 32767.0f);
    out[1] = (int16_t)(r * 32767.0f);
}

/* ------------------------------------------------------------------ samples */
struct AudSample {
    uint32_t id;
    char     path[256];
    int16_t *pcm;      /* interleaved, the sample's own rate */
    int      frames, ch, rate;
    bool     from_file;
};

#define MAX_SAMPLES 160
static AudSample samples[MAX_SAMPLES];
static int nsamples;

AudSample *aud_sample_pack(uint32_t id)
{
    for (int i = 0; i < nsamples; i++)
        if (!samples[i].from_file && samples[i].id == id) return samples[i].pcm ? &samples[i] : NULL;
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++];
    memset(s, 0, sizeof *s);
    s->id = id;
    const PackEntry *e = packs_find(id);
    if (e) s->pcm = sfx_decode_pcm(e->data, e->size, &s->frames, &s->ch);
    if (s->pcm && s->rate == 0) s->rate = 44100;
    return s->pcm ? s : NULL;
}

AudSample *aud_sample_file(const char *path)
{
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < nsamples; i++) if (samples[i].from_file && !strcmp(samples[i].path, path)) return samples[i].pcm ? &samples[i] : NULL;
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++];
    memset(s, 0, sizeof *s);
    s->from_file = true;
    snprintf(s->path, sizeof s->path, "%s", path);
    /* the same RIFF walk aud_sdl.c does: fmt (PCM 1, 16-bit) then data */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 0 ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(d); return NULL; }
    fclose(f);
    int ch = 1, rate = 44100, bits = 16;
    const uint8_t *pcm = NULL;
    uint32_t n = 0;
    if (size > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        size_t p = 12;
        while (p + 8 <= (size_t)size) {
            uint32_t len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
            if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
                ch = d[p + 10] | d[p + 11] << 8;
                rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16 | (uint32_t)d[p + 15] << 24;
                bits = d[p + 22] | d[p + 23] << 8;
            } else if (!memcmp(d + p, "data", 4)) {
                pcm = d + p + 8;
                n = len;
                if (p + 8 + n > (size_t)size) n = (uint32_t)(size - p - 8);
                break;
            }
            p += 8 + len + (len & 1);
        }
    }
    if (pcm && bits == 16 && ch >= 1 && ch <= 2) {
        s->pcm = malloc(n);
        if (s->pcm) {
            memcpy(s->pcm, pcm, n);
            s->frames = (int)(n / 2 / ch);
            s->ch = ch;
            s->rate = rate > 0 ? rate : 44100;
        }
    }
    free(d);
    return s->pcm ? s : NULL;
}

void aud_keep(AudSample *s, bool loop) { (void)s; (void)loop; }   /* a sample is resident as soon as it is looked up */
void aud_prefetch(AudSample *s) { (void)s; }
void aud_unkeep(AudSample *s) { (void)s; }

/* ------------------------------------------------------------------ voices
 * A voice plays one sample, resampled from its own rate to the output rate (most are already 44100, so the step
 * is 1.0 and the inner loop is a copy), with the handle a generation counter packed in, as the other backends do:
 * the core holds the handle across a stop and a restart, and a recycled slot must read as a different voice. */
#define MAX_VOICES 16
static struct {
    AudSample *smp;
    double     pos;      /* the read position in the source, in frames */
    double     step;     /* source frames per output frame */
    float      gain;
    bool       loop, active;
    unsigned   gen;
} voices[MAX_VOICES];

static int handle_of(int v) { return (int)(voices[v].gen << 5) | v; }
static int voice_of(int h)
{
    if (h < 0) return -1;
    int v = h & 31;
    return v < MAX_VOICES && (int)(voices[v].gen << 5 | v) == h ? v : -1;
}

int aud_play(AudSample *s, real gain, bool loop)
{
    if (!s || !s->pcm || s->frames <= 0) return -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].active) continue;
        voices[v].smp = s;
        voices[v].pos = 0;
        voices[v].step = (double)s->rate / (double)WASM_AUDIO_RATE;
        voices[v].gain = gain;
        voices[v].loop = loop;
        voices[v].active = true;
        voices[v].gen++;
        return handle_of(v);
    }
    return -1;   /* every voice busy: the core copes with a dropped effect (as it does on the consoles) */
}
void aud_set_gain(int h, real gain) { int v = voice_of(h); if (v >= 0) voices[v].gain = gain; }
void aud_stop(int h)
{
    int v = voice_of(h);
    if (v < 0) return;
    voices[v].active = false;
    voices[v].gen++;
}
bool aud_playing(int h) { int v = voice_of(h); return v >= 0 && voices[v].active; }

/* mix one voice into the output frame pair; the source is read with linear interpolation between the two frames
 * it straddles, which is inaudible at these rates and keeps the inner loop branch-free. The accumulator is -1..1:
 * an S16 sample is 1/32768 of full scale, and the voice gain is the core's linear bus level. */
static inline void mix_voice(int v, float *l, float *r)
{
    AudSample *s = voices[v].smp;
    double p = voices[v].pos;
    int i0 = (int)p;
    double fr = p - i0;
    if (i0 < 0) { i0 = 0; fr = 0; }
    if (i0 >= s->frames) {
        if (voices[v].loop) {
            voices[v].pos = 0;
            p = 0;
            i0 = 0;
            fr = 0;
        } else {
            voices[v].active = false;
            return;
        }
    }
    int n0 = i0 * s->ch, n1 = (i0 + 1 < s->frames ? i0 + 1 : i0) * s->ch;
    float sl, sr;
    if (s->ch >= 2) {
        sl = (float)(s->pcm[n0] * (1.0 - fr) + s->pcm[n1] * fr) * (1.0f / 32768.0f);
        sr = (float)(s->pcm[n0 + 1] * (1.0 - fr) + s->pcm[n1 + 1] * fr) * (1.0f / 32768.0f);
    } else {
        sl = sr = (float)(s->pcm[n0] * (1.0 - fr) + s->pcm[n1] * fr) * (1.0f / 32768.0f);
    }
    *l += sl * voices[v].gain;
    *r += sr * voices[v].gain;
    voices[v].pos = p + voices[v].step;
    if (voices[v].pos >= s->frames && !voices[v].loop) voices[v].active = false;
}

/* ------------------------------------------------------------------ music (MUPS -> Ogg Vorbis) */
static stb_vorbis *mus_vorbis;
static uint8_t *mus_ogg;
static size_t mus_ogg_len;
static int mus_rate, mus_ch;
static float mus_gain = 1.0f;
static bool mus_loop, mus_paused;
static double mus_pos;             /* the fractional source frame being read (rate conversion below) */
static double mus_step;            /* source frames per output frame */
#define MUS_BUF_FRAMES 2048
static short mus_buf[MUS_BUF_FRAMES * 2];
static int mus_buf_frames;         /* decoded source frames held in mus_buf */

/* drop the source frames already played, so a refill appends after what is left */
static void music_compact(void)
{
    int done = (int)mus_pos;
    if (done <= 0) return;
    if (done >= mus_buf_frames) { mus_buf_frames = 0; mus_pos = 0; return; }
    memmove(mus_buf, mus_buf + (size_t)done * mus_ch, (size_t)(mus_buf_frames - done) * mus_ch * sizeof *mus_buf);
    mus_buf_frames -= done;
    mus_pos -= done;
}

static void music_stop_now(void)
{
    if (mus_vorbis) { stb_vorbis_close(mus_vorbis); mus_vorbis = NULL; }
    free(mus_ogg);
    mus_ogg = NULL;
    mus_ogg_len = 0;
    mus_buf_frames = 0;
    mus_pos = 0;
}

void aud_music_stop(void) { music_stop_now(); }

static bool music_open(const uint8_t *mups, uint32_t size, bool loop)
{
    music_stop_now();
    if (!mups || !size) return false;
    size_t len = 0;
    mus_ogg = mups_to_ogg(mups, size, &len);
    if (!mus_ogg) return false;
    mus_ogg_len = len;
    int err = 0;
    mus_vorbis = stb_vorbis_open_memory(mus_ogg, (int)len, &err, NULL);
    if (!mus_vorbis) {
        fprintf(stderr, "music: stb_vorbis_open_memory failed (%d)\n", err);
        free(mus_ogg);
        mus_ogg = NULL;
        return false;
    }
    stb_vorbis_info vi = stb_vorbis_get_info(mus_vorbis);
    mus_rate = vi.sample_rate;
    mus_ch = vi.channels;
    mus_loop = loop;
    mus_paused = false;
    mus_pos = 0;
    mus_step = mus_rate > 0 ? (double)mus_rate / (double)WASM_AUDIO_RATE : 1.0;
    mus_buf_frames = 0;
    return true;
}

bool aud_music_play(uint32_t id, bool loop)
{
    const PackEntry *e = packs_find(id);
    return e && music_open(e->data, e->size, loop);
}
void aud_music_gain(real g) { mus_gain = g; }
void aud_music_pause(bool pause) { mus_paused = pause; }

/* the decoded block, refilled as it is consumed. mus_buf holds whole source frames (mus_ch shorts each);
 * stb_vorbis reports decoded frames per channel, so the short count is got * mus_ch. */
static void music_fill(void)
{
    if (!mus_vorbis) return;
    music_compact();
    int room = MUS_BUF_FRAMES - mus_buf_frames;
    if (room <= 0) return;
    int got = stb_vorbis_get_samples_short_interleaved(mus_vorbis, mus_ch,
                                                       mus_buf + (size_t)mus_buf_frames * mus_ch,
                                                       room * mus_ch);
    if (got <= 0) {
        if (mus_loop) {
            stb_vorbis_seek(mus_vorbis, 0);
            got = stb_vorbis_get_samples_short_interleaved(mus_vorbis, mus_ch,
                                                           mus_buf + (size_t)mus_buf_frames * mus_ch,
                                                           room * mus_ch);
        }
        if (got <= 0) { music_stop_now(); return; }
    }
    mus_buf_frames += got;
}

static inline void mix_music(float *l, float *r)
{
    if (!mus_vorbis || mus_paused) return;
    /* keep a source frame either side of the read position buffered */
    while (mus_vorbis && (int)(mus_pos + 1) >= mus_buf_frames) {
        int before = mus_buf_frames;
        music_fill();
        if (!mus_vorbis || mus_buf_frames == before) break;
    }
    if (!mus_vorbis) return;
    int i0 = (int)mus_pos;
    double fr = mus_pos - i0;
    if (i0 < 0) { i0 = 0; fr = 0; }
    if (i0 + 1 >= mus_buf_frames) {
        /* at the very end of a non-looping track: hold the last frame rather than going silent mid-sample */
        if (i0 >= mus_buf_frames) return;
        fr = 0;
    }
    float ml, mr;
    if (mus_ch >= 2) {
        ml = (float)(mus_buf[i0 * mus_ch] * (1.0 - fr) + mus_buf[(i0 + 1) * mus_ch] * fr);
        mr = (float)(mus_buf[i0 * mus_ch + 1] * (1.0 - fr) + mus_buf[(i0 + 1) * mus_ch + 1] * fr);
    } else {
        ml = mr = (float)(mus_buf[i0 * mus_ch] * (1.0 - fr) + mus_buf[(i0 + 1) * mus_ch] * fr);
    }
    ml *= mus_gain * (1.0f / 32768.0f);
    mr *= mus_gain * (1.0f / 32768.0f);
    *l += ml;
    *r += mr;
    mus_pos += mus_step;
}

/* ------------------------------------------------------------------ the mix
 * audio_update runs once per 60 Hz game step (audio.c), so one step's worth of output frames is mixed here;
 * the page drains the ring every animation frame. */
void aud_update(void)
{
    audio_pump(WASM_AUDIO_RATE / 60);
}

void audio_pump(int frames)
{
    for (int f = 0; f < frames; f++) {
        float l = 0, r = 0;
        for (int v = 0; v < MAX_VOICES; v++) if (voices[v].active) mix_voice(v, &l, &r);
        mix_music(&l, &r);
        int16_t out[2];
        limit_and_store(l, r, out);
        ring_push(out[0], out[1]);
    }
}

float audio_peak(void) { return lim_env; }

bool aud_init(void)
{
    ring_read = ring_write = ring_count = 0;
    lim_env = lim_release = 0;
    return true;
}

void aud_shutdown(void)
{
    music_stop_now();
    for (int v = 0; v < MAX_VOICES; v++) voices[v].active = false;
}
