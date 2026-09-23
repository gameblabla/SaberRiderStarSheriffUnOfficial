/* platform/aud.h on SDL3 audio streams: every voice is a stream bound to one device, music is decoded with
 * libvorbisfile from the packs' MUPS (renamed Ogg Vorbis), and a peak limiter runs on the device's post-mix. */
#include "../aud.h"
#include "../common/sfx_decode.h"
#include "../common/mups.h"
#include "../../pack.h"
#include "sdl_platform.h"
#include <vorbis/vorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_AudioDeviceID dev;
static SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 44100 };

/* ---- the limiter: the core leaves each bus some headroom; the sum never clips */
#define LIMIT_CEIL 0.89f   /* limiter ceiling (-1 dBFS) */
#define LIMIT_RELEASE_S 0.12f
static struct { float env, release; } lim;

static void SDLCALL postmix(void *ud, const SDL_AudioSpec *sp, float *buf, int buflen)
{
    (void)ud;
    int ch = sp->channels > 0 ? sp->channels : 1, n = buflen / (int)sizeof(float) / ch;
    if (lim.release == 0) lim.release = SDL_expf(-1.0f / (LIMIT_RELEASE_S * (float)sp->freq));
    float env = lim.env, rel = lim.release;
    for (int i = 0; i < n; i++) {
        float *f = buf + i * ch, peak = 0;
        for (int c = 0; c < ch; c++) { float a = SDL_fabsf(f[c]); if (a > peak) peak = a; }
        env = peak > env ? peak : env * rel;          /* instant attack, exponential release */
        float g = env > LIMIT_CEIL ? LIMIT_CEIL / env : 1.0f;
        for (int c = 0; c < ch; c++) {
            float v = f[c] * g;
            f[c] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;   /* safety only: g keeps |v| <= ceiling */
        }
    }
    lim.env = env;
}

/* ---- samples ---- */
struct AudSample { uint32_t id; char path[256]; int16_t *pcm; int frames, ch, rate; };
#define MAX_SAMPLES 160
static AudSample samples[MAX_SAMPLES]; static int nsamples;

AudSample *aud_sample_pack(uint32_t id)
{
    for (int i = 0; i < nsamples; i++) if (!samples[i].path[0] && samples[i].id == id) return samples[i].pcm ? &samples[i] : NULL;
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++]; memset(s, 0, sizeof *s); s->id = id; s->rate = 44100;
    const PackEntry *e = packs_find(id);
    if (e) s->pcm = sfx_decode_pcm(e->data, e->size, &s->frames, &s->ch);
    return s->pcm ? s : NULL;
}

AudSample *aud_sample_file(const char *path)
{
    if (!path) return NULL;
    for (int i = 0; i < nsamples; i++) if (!strcmp(samples[i].path, path)) return samples[i].pcm ? &samples[i] : NULL;
    if (nsamples == MAX_SAMPLES) return NULL;
    AudSample *w = &samples[nsamples++]; memset(w, 0, sizeof *w); snprintf(w->path, sizeof w->path, "%s", path);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = size > 0 ? malloc((size_t)size) : NULL;
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(d); return NULL; }
    fclose(f);
    int ch = 1, rate = 44100, bits = 16; const uint8_t *pcm = NULL; uint32_t n = 0;
    if (size > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {   /* fmt (PCM 1, 16-bit) then data */
        size_t p = 12;
        while (p + 8 <= (size_t)size) {
            uint32_t len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
            if (!memcmp(d + p, "fmt ", 4) && len >= 16) { ch = d[p + 10] | d[p + 11] << 8; rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16 | d[p + 15] << 24; bits = d[p + 22] | d[p + 23] << 8; }
            else if (!memcmp(d + p, "data", 4)) { pcm = d + p + 8; n = len; if (p + 8 + n > (size_t)size) n = (uint32_t)(size - p - 8); break; }
            p += 8 + len + (len & 1);
        }
    }
    if (pcm && bits == 16 && ch >= 1 && ch <= 2) { w->pcm = malloc(n); memcpy(w->pcm, pcm, n); w->frames = (int)(n / 2 / ch); w->ch = ch; w->rate = rate; }
    else fprintf(stderr, "%s: not a PCM16 wav\n", path);
    free(d);
    return w->pcm ? w : NULL;
}

/* ---- voices ---- */
#define MAX_VOICES 16
static struct { SDL_AudioStream *st; AudSample *smp; bool loop; unsigned gen; } voices[MAX_VOICES];
static int handle_of(int v) { return (int)(voices[v].gen << 5) | v; }
static int voice_of(int h) { if (h < 0) return -1; int v = h & 31; return v < MAX_VOICES && (int)(voices[v].gen << 5 | v) == h ? v : -1; }

static int play_pcm(const int16_t *pcm, int frames, int ch, int rate, float gain, bool loop, AudSample *s)
{
    if (!dev || !pcm) return -1;
    SDL_AudioSpec in = { SDL_AUDIO_S16, ch, rate };
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].st && (voices[v].loop || SDL_GetAudioStreamQueued(voices[v].st) > 0)) continue;
        if (voices[v].st) SDL_DestroyAudioStream(voices[v].st);
        voices[v].st = SDL_CreateAudioStream(&in, &spec);
        if (!voices[v].st) return -1;
        voices[v].smp = s; voices[v].loop = loop; voices[v].gen++;
        SDL_SetAudioStreamGain(voices[v].st, gain);
        SDL_BindAudioStream(dev, voices[v].st);
        SDL_PutAudioStreamData(voices[v].st, pcm, frames * ch * 2);
        if (!loop) SDL_FlushAudioStream(voices[v].st);
        return handle_of(v);
    }
    return -1;
}

int aud_play(AudSample *s, float gain, bool loop) { return s ? play_pcm(s->pcm, s->frames, s->ch, s->rate, gain, loop, s) : -1; }
void aud_set_gain(int h, float gain) { int v = voice_of(h); if (v >= 0 && voices[v].st) SDL_SetAudioStreamGain(voices[v].st, gain); }
void aud_stop(int h)
{
    int v = voice_of(h); if (v < 0 || !voices[v].st) return;
    SDL_ClearAudioStream(voices[v].st); voices[v].loop = false; voices[v].gen++;
}
bool aud_playing(int h) { int v = voice_of(h); return v >= 0 && voices[v].st && (voices[v].loop || SDL_GetAudioStreamQueued(voices[v].st) > 0); }

/* ---- music (MUPS -> Ogg in memory, streamed with vorbisfile) ---- */
static SDL_AudioStream *music_stream;
static uint8_t *ogg_buf; static size_t ogg_len, ogg_pos;
static OggVorbis_File vf; static bool music_open, music_loop;
static float music_gain = 1.0f;

static size_t cb_read(void *ptr, size_t sz, size_t nm, void *src) { (void)src; size_t want = sz * nm, left = ogg_len - ogg_pos; if (want > left) want = left; memcpy(ptr, ogg_buf + ogg_pos, want); ogg_pos += want; return want / (sz ? sz : 1); }
static int cb_seek(void *src, ogg_int64_t off, int whence) { (void)src; size_t np = whence == SEEK_SET ? (size_t)off : whence == SEEK_CUR ? ogg_pos + off : ogg_len + off; if (np > ogg_len) return -1; ogg_pos = np; return 0; }
static long cb_tell(void *src) { (void)src; return (long)ogg_pos; }

void aud_music_stop(void)
{
    if (music_open) { ov_clear(&vf); music_open = false; }
    if (music_stream) { SDL_DestroyAudioStream(music_stream); music_stream = NULL; }
    free(ogg_buf); ogg_buf = NULL;
}

static void music_fill(void)
{
    if (!music_open || !music_stream) return;
    float buf[4096]; float **pcm; int sec, ch = ov_info(&vf, -1)->channels;
    while (SDL_GetAudioStreamQueued(music_stream) < 44100 * 8 / 2) {   /* keep ~0.5 s queued (f32 stereo) */
        long n = ov_read_float(&vf, &pcm, (int)(sizeof buf / sizeof *buf) / ch, &sec);   /* unclipped: tracks peak > 0 dBFS */
        if (n <= 0) { if (n == 0 && music_loop) { ov_raw_seek(&vf, 0); continue; } break; }
        for (long i = 0; i < n; i++) for (int c = 0; c < ch; c++) buf[i * ch + c] = pcm[c][i];
        SDL_PutAudioStreamData(music_stream, buf, (int)(n * ch * sizeof(float)));
    }
}

static bool music_open_mups(const uint8_t *mups, uint32_t size, bool loop)
{
    aud_music_stop();
    if (!dev) return false;
    ogg_buf = mups_to_ogg(mups, size, &ogg_len); ogg_pos = 0;
    if (!ogg_buf) return false;
    ov_callbacks cb = { cb_read, cb_seek, NULL, cb_tell };
    int rc = ov_open_callbacks(&ogg_pos, &vf, NULL, 0, cb);   /* datasource must be non-NULL */
    if (rc < 0) { fprintf(stderr, "music: ov_open failed rc=%d len=%zu\n", rc, ogg_len); free(ogg_buf); ogg_buf = NULL; return false; }
    music_open = true; music_loop = loop;
    vorbis_info *vi = ov_info(&vf, -1);
    SDL_AudioSpec in = { SDL_AUDIO_F32, vi->channels, (int)vi->rate };
    music_stream = SDL_CreateAudioStream(&in, &spec);
    SDL_BindAudioStream(dev, music_stream);
    SDL_SetAudioStreamGain(music_stream, music_gain);
    music_fill();
    return true;
}

bool aud_music_play(uint32_t id, bool loop)
{
    const PackEntry *e = packs_find(id);
    return e && music_open_mups(e->data, e->size, loop);
}
void aud_music_gain(float g) { music_gain = g; if (music_stream) SDL_SetAudioStreamGain(music_stream, g); }
void aud_music_pause(bool pause)
{
    if (!music_stream) return;
    if (pause) SDL_PauseAudioStreamDevice(music_stream); else SDL_ResumeAudioStreamDevice(music_stream);
}

/* ---- a video's own soundtrack ---- */
bool snd_sdl_music_blob(const uint8_t *mups, uint32_t size, bool loop) { return music_open_mups(mups, size, loop); }
static AudSample blob; static int blob_voice = -1;
void snd_sdl_sfx_blob(const uint8_t *riff, uint32_t size)
{
    snd_sdl_stop_blob();
    free(blob.pcm); memset(&blob, 0, sizeof blob); blob.rate = 44100;
    blob.pcm = sfx_decode_pcm(riff, size, &blob.frames, &blob.ch);
    if (blob.pcm) blob_voice = aud_play(&blob, 0.80f, false);   /* the core's voice bus level */
}
void snd_sdl_stop_blob(void) { if (blob_voice >= 0) { aud_stop(blob_voice); blob_voice = -1; } }

/* ---- lifetime ---- */
void aud_update(void)
{
    for (int v = 0; v < MAX_VOICES; v++)   /* loops: keep ~0.25 s queued */
        if (voices[v].st && voices[v].loop && voices[v].smp)
            while (SDL_GetAudioStreamQueued(voices[v].st) < (int)(voices[v].smp->rate * voices[v].smp->ch * 2 * 0.25f))
                SDL_PutAudioStreamData(voices[v].st, voices[v].smp->pcm, voices[v].smp->frames * voices[v].smp->ch * 2);
    music_fill();
}

bool aud_init(void)
{
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return false; }
    SDL_SetAudioPostmixCallback(dev, postmix, NULL);
    return true;
}

void aud_shutdown(void)
{
    aud_music_stop();
    for (int v = 0; v < MAX_VOICES; v++) if (voices[v].st) SDL_DestroyAudioStream(voices[v].st);
    if (dev) SDL_CloseAudioDevice(dev);
}
