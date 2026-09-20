#include "audio.h"
#include "pack.h"
#include "lzo1z.h"
#include <SDL3/SDL.h>
#include <vorbis/vorbisfile.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static SDL_AudioDeviceID dev;
static SDL_AudioStream *music_stream;
static SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 44100 };

/* ---- mix bus ----
 * The original (FUN_00563900 / FUN_00563780) sums the music at vol/256 and every sfx voice at unity into 16-bit
 * and hard-clips at +-0x7fbc. The material is mastered hot (most sfx peak at 0 dBFS, the music tracks at
 * 0..+1 dB), so a voice line over a gunshot over the level track clips audibly. Instead of reproducing that,
 * each bus gets some headroom and the device's post-mix pass runs a peak limiter, so the sum never clips. */
#define GAIN_SFX   0.60f   /* game sfx: -4.4 dB */
#define GAIN_VOICE 0.80f   /* dialog / video speech: -1.9 dB */
#define GAIN_MUSIC 0.90f   /* on top of the music table's per-track volume and the fade */
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

/* ---- sfx ---- */
static const uint32_t SFX_TABLE[32] = {   /* 0x7c5480 */
    0xE418A101, 0xEB3309DC, 0xEB450AED, 0x8ADE82B6, 0x8ACB81A0, 0x8AB88092, 0x9C7B3FD9, 0xC66E1894,
    0xC6801BB9, 0xBF4917FF, 0xBF5B14EA, 0xBF6D1599, 0x89389611, 0x8923950D, 0xFE10EB78, 0xFDFFE848,
    0xA8382083, 0x162864B4, 0x8BE8F136, 0xF8B5C0E8, 0x208C64D9, 0xE73A3850, 0x87265BA0, 0x1DBF470E,
    0x8AEB8147, 0xF11FCC31, 0x0AFC505A, 0x15A00BA1, 0x82EFBA26, 0x47D886A1, 0xE105C92A, 0xABC6A6E8 };
static const uint32_t MUSIC_TABLE[18] = {   /* 0x7c53a0 */
    0xD9F22466, 0x70090142, 0x39E19337, 0x086C02EE, 0x3E0E8A02, 0xEABD3205, 0xAF162C58, 0xC93B9F7F, 0xB10AB207,
    0x25930DD9, 0xD50E6105, 0xD518620C, 0xD52263F3, 0xD52C64FE, 0xD53665E9, 0xD54066D0, 0xD54A67DF, 0xD55468CA };

typedef struct { int16_t *pcm; int frames; int channels; } Sfx;
static Sfx sfx_cache[32];
#define MAX_VOICES 16
static SDL_AudioStream *voices[MAX_VOICES];
typedef struct { int table_idx; int frame; } Delayed;
static Delayed delayed[64]; static int ndelayed;
static uint32_t frame_counter;

static const int T1[16] = {1,3,5,7,9,11,13,15,-1,-3,-5,-7,-9,-11,-13,-15};
static const int T2[8] = {230,230,230,230,307,409,512,614};

static Sfx *load_sfx_entry(const PackEntry *e, Sfx *s)
{
    if (s->pcm) return s;
    if (!e || e->size < 44) return NULL;
    const uint8_t *d = e->data;
    uint16_t fmt = d[20] | d[21] << 8, ch = d[22] | d[23] << 8;
    uint32_t claim = d[40] | d[41] << 8 | d[42] << 16 | (uint32_t)d[43] << 24;
    uint8_t *raw = malloc(claim + 16);
    int n = lzo1z_decompress(d + 44, e->size - 44, raw, claim + 16);
    if (n < 0) { memcpy(raw, d + 44, e->size - 44 < claim ? e->size - 44 : claim); n = (int)claim; }
    if (fmt == 0xF423) {          /* Yamaha ADPCM, mono */
        s->frames = n * 2; s->channels = 1;
        s->pcm = malloc((size_t)s->frames * 2);
        int step = 0x7f, prev = 0, k = 0;
        for (int i = 0; i < n; i++) {
            for (int h = 0; h < 2; h++) {
                int nib = h ? (raw[i] >> 4) & 15 : raw[i] & 15;
                int prod = T1[nib] * step;
                int v = (prod >= 0 ? (prod + 7) >> 3 : prod >> 3) + prev;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                s->pcm[k++] = (int16_t)v; prev = v;
                step = (step * T2[nib & 7]) >> 8;
                if (step > 0x6000) step = 0x6000;
                if (step <= 0x7e) step = 0x7f;
            }
        }
    } else {                      /* 0xF424: PCM16 */
        s->channels = ch ? ch : 2; s->frames = n / 2 / s->channels;
        s->pcm = malloc((size_t)n); memcpy(s->pcm, raw, n);
    }
    free(raw);
    return s;
}

static Sfx *load_sfx(int idx) { return load_sfx_entry(packs_find(SFX_TABLE[idx]), &sfx_cache[idx]); }

static SDL_AudioStream *play_sfx(Sfx *s, float gain)
{
    if (!dev || !s || !s->pcm) return NULL;
    SDL_AudioSpec in = { SDL_AUDIO_S16, s->channels, 44100 };
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v] && SDL_GetAudioStreamQueued(voices[v]) > 0) continue;
        if (voices[v]) SDL_DestroyAudioStream(voices[v]);
        voices[v] = SDL_CreateAudioStream(&in, &spec);
        if (!voices[v]) return NULL;
        SDL_SetAudioStreamGain(voices[v], gain);
        SDL_BindAudioStream(dev, voices[v]);
        SDL_PutAudioStreamData(voices[v], s->pcm, s->frames * s->channels * 2);
        SDL_FlushAudioStream(voices[v]);
        return voices[v];
    }
    return NULL;
}

#define MAX_EXTRA 16
static struct { uint32_t id; Sfx s; } extra[MAX_EXTRA]; static int nextra;
void sfx_play_id(uint32_t id)
{
    for (int i = 0; i < nextra; i++) if (extra[i].id == id) { play_sfx(&extra[i].s, GAIN_VOICE); return; }
    if (nextra == MAX_EXTRA) return;
    extra[nextra].id = id; memset(&extra[nextra].s, 0, sizeof(Sfx));
    if (load_sfx_entry(packs_find(id), &extra[nextra].s)) play_sfx(&extra[nextra].s, GAIN_VOICE);
    nextra++;
}

/* FUN_00425880 / FUN_004109a0: every table sample owns one slot. A request opens (or extends) a window of
 * len*3 ms; inside a window the sample plays at most once per len*6 ms, so hammering the trigger (auto-fire,
 * a burst of hits) does not stack identical copies or eat all 16 voices, which is what starved other sounds. */
static const int SFX_LEN[32] = {   /* 0x7c5484: retrigger length units */
    40, 60, 60, 60, 60, 60, 40, 36, 36, 16, 16, 16, 20, 20, 12, 12,
    40, 40, 40, 40, 40, 20, 40, 40, 36, 18, 36, 60, 200, 60, 60, 50 };
static struct { uint32_t end, next; } slot[32];

static void service_slots(void)
{
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < 32; i++) {
        if (!slot[i].end) continue;
        if ((int32_t)(now - slot[i].end) >= 0) { slot[i].end = 0; continue; }
        if ((int32_t)(now - slot[i].next) >= 0) { play_sfx(load_sfx(i), GAIN_SFX); slot[i].next = now + SFX_LEN[i] * 6; }
    }
}

static void play_table(int idx)
{
    if (idx < 0 || idx >= 32) return;
    uint32_t now = SDL_GetTicks();
    if (!slot[idx].end) slot[idx].next = now;
    slot[idx].end = now + SFX_LEN[idx] * 3 - 3;
    service_slots();
}

static int rnd(int n) { return rand() % (n + 1); }   /* FUN_0040cf30(0, n) inclusive per the switch usage */

void sfx_play(int id, int delay)
{
    int t = -1, t2 = -1;
    switch (id) {
    case 0: t = 0; break;
    case 1: t = 7; if (rnd(100) > 0x50) t2 = 8; break;
    case 2: t = 6; break;
    case 3: { int r = rnd(2); t = r == 0 ? 3 : r == 1 ? 4 : 5; break; }
    case 4: t = rnd(1) == 0 ? 2 : 1; break;
    case 5: { int r = rnd(2); t = r == 0 ? 9 : r == 1 ? 10 : 11; break; }
    case 6: t = rnd(1) == 0 ? 13 : 12; break;
    case 7: t = rnd(1) == 0 ? 15 : 14; break;
    default: if (id >= 8 && id <= 23) t = id + 8; break;
    }
    if (t < 0) return;
    if (delay > 0 && ndelayed < 64) { delayed[ndelayed].table_idx = t; delayed[ndelayed].frame = frame_counter + delay; ndelayed++; }
    else play_table(t);
    if (t2 >= 0) play_table(t2);
}

/* ---- music (MUPS -> Ogg in memory, streamed with vorbisfile) ---- */
static uint8_t *ogg_buf; static size_t ogg_len, ogg_pos;
static OggVorbis_File vf; static bool music_open, music_loop;
static float music_track_vol = 1.0f, music_fade = 1.0f;   /* 0x7c53a8 per-track volume x FUN_00425e70 fade */
static const float MUSIC_VOL[18] = { 1, 1, 1, 1, 1, 0.85f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

static void apply_music_gain(void)
{
    /* the mixer's 0..255 stream volume is not linear: a 1 s fade in the original is inaudible after ~0.7 s,
     * which a squared curve reproduces (measured on a pulse capture of the original's character select) */
    float v = music_fade < 0.001f ? 0 : music_fade > 0.999f ? 1.0f : music_fade;
    if (music_stream) SDL_SetAudioStreamGain(music_stream, v * v * music_track_vol * GAIN_MUSIC);
}

void music_set_volume(float v) { music_fade = v < 0 ? 0 : v > 1 ? 1 : v; apply_music_gain(); }
void music_pause(bool pause)
{
    if (!music_stream) return;
    if (pause) SDL_PauseAudioStreamDevice(music_stream); else SDL_ResumeAudioStreamDevice(music_stream);
}

static uint32_t ogg_crc_table[256];
static void crc_init(void) { for (uint32_t i = 0; i < 256; i++) { uint32_t r = i << 24; for (int k = 0; k < 8; k++) r = (r & 0x80000000) ? (r << 1) ^ 0x04C11DB7 : r << 1; ogg_crc_table[i] = r; } }
static uint32_t ogg_crc(const uint8_t *p, size_t n) { uint32_t c = 0; for (size_t i = 0; i < n; i++) c = (c << 8) ^ ogg_crc_table[((c >> 24) & 0xff) ^ p[i]]; return c; }

static uint8_t *mups_to_ogg(const uint8_t *d, size_t n, size_t *out_len)
{
    if (n < 8 || memcmp(d, "MUPS", 4)) return NULL;
    uint8_t *out = malloc(n); size_t o = 0, i = 8;
    while (i + 27 <= n && !memcmp(d + i, "PssH", 4)) {
        int nseg = d[i + 26]; size_t plen = 0;
        for (int k = 0; k < nseg; k++) plen += d[i + 27 + k];
        size_t page = 27 + nseg + plen;
        if (i + page > n) break;
        memcpy(out + o, d + i, page);
        memcpy(out + o, "OggS", 4);
        uint8_t *pay = out + o + 27 + nseg;
        /* rename the codec id at every packet start within the page */
        for (size_t st = 0, k = 0; k <= (size_t)nseg; k++) {
            if (st + 7 <= plen && (pay[st] == 1 || pay[st] == 3 || pay[st] == 5) && !memcmp(pay + st + 1, "2Dream", 6)) memcpy(pay + st + 1, "vorbis", 6);
            if (k == (size_t)nseg) break;
            st += d[i + 27 + k];
            if (d[i + 27 + k] == 255) { /* continue within the same packet: skip until a short lacing value */
                while (k + 1 < (size_t)nseg && d[i + 27 + k] == 255) { k++; st += d[i + 27 + k]; }
            }
        }
        memset(out + o + 22, 0, 4);
        uint32_t c = ogg_crc(out + o, page);
        out[o + 22] = c & 0xff; out[o + 23] = (c >> 8) & 0xff; out[o + 24] = (c >> 16) & 0xff; out[o + 25] = c >> 24;
        o += page; i += page;
    }
    *out_len = o;
    return out;
}

static size_t cb_read(void *ptr, size_t sz, size_t nm, void *src) { (void)src; size_t want = sz * nm, left = ogg_len - ogg_pos; if (want > left) want = left; memcpy(ptr, ogg_buf + ogg_pos, want); ogg_pos += want; return want / (sz ? sz : 1); }
static int cb_seek(void *src, ogg_int64_t off, int whence) { (void)src; size_t np = whence == SEEK_SET ? (size_t)off : whence == SEEK_CUR ? ogg_pos + off : ogg_len + off; if (np > ogg_len) return -1; ogg_pos = np; return 0; }
static long cb_tell(void *src) { (void)src; return (long)ogg_pos; }

void music_stop(void)
{
    if (music_open) { ov_clear(&vf); music_open = false; }
    if (music_stream) { SDL_DestroyAudioStream(music_stream); music_stream = NULL; }
    free(ogg_buf); ogg_buf = NULL;
}

bool music_play_blob(const uint8_t *mups, uint32_t size, bool loop)
{
    music_stop();
    if (!dev) return false;
    ogg_buf = mups_to_ogg(mups, size, &ogg_len); ogg_pos = 0;
    if (!ogg_buf) return false;
    ov_callbacks cb = { cb_read, cb_seek, NULL, cb_tell };
    int rc = ov_open_callbacks(&ogg_pos, &vf, NULL, 0, cb);
    if (rc < 0) { fprintf(stderr, "music blob: ov_open failed rc=%d\n", rc); return false; }
    music_open = true; music_loop = loop;
    vorbis_info *vi = ov_info(&vf, -1);
    SDL_AudioSpec in = { SDL_AUDIO_F32, vi->channels, (int)vi->rate };
    music_stream = SDL_CreateAudioStream(&in, &spec);
    SDL_BindAudioStream(dev, music_stream);
    music_track_vol = 1.0f; music_fade = 1.0f; apply_music_gain();
    audio_update();
    return true;
}

static Sfx blob_sfx; static SDL_AudioStream *blob_voice;
void sfx_play_blob(const uint8_t *riff, uint32_t size)
{
    PackEntry tmp = { 0, RES_SFX, riff, size, false };
    sfx_stop_blob();
    if (blob_sfx.pcm) { free(blob_sfx.pcm); memset(&blob_sfx, 0, sizeof blob_sfx); }
    if (load_sfx_entry(&tmp, &blob_sfx)) blob_voice = play_sfx(&blob_sfx, GAIN_VOICE);
}
void sfx_stop_blob(void)
{
    if (blob_voice) { SDL_ClearAudioStream(blob_voice); blob_voice = NULL; }
}

void music_play(int index, bool loop)
{
    music_stop();
    if (!dev || index < 0 || index >= 18) return;
    const PackEntry *e = packs_find(MUSIC_TABLE[index]);
    if (!e) return;
    ogg_buf = mups_to_ogg(e->data, e->size, &ogg_len); ogg_pos = 0;
    if (!ogg_buf) return;
    ov_callbacks cb = { cb_read, cb_seek, NULL, cb_tell };
    int rc = ov_open_callbacks(&ogg_pos, &vf, NULL, 0, cb);   /* datasource must be non-NULL */
    if (rc < 0) { fprintf(stderr, "music %d: ov_open failed rc=%d len=%zu head=%02x%02x%02x%02x\n", index, rc, ogg_len, ogg_buf[0], ogg_buf[1], ogg_buf[2], ogg_buf[3]); return; }
    music_open = true; music_loop = loop;
    vorbis_info *vi = ov_info(&vf, -1);
    SDL_AudioSpec in = { SDL_AUDIO_F32, vi->channels, (int)vi->rate };
    music_stream = SDL_CreateAudioStream(&in, &spec);
    SDL_BindAudioStream(dev, music_stream);
    music_track_vol = MUSIC_VOL[index]; music_fade = 1.0f; apply_music_gain();
    audio_update();
}

void audio_update(void)
{
    frame_counter++;
    for (int i = 0; i < ndelayed; ) {
        if ((int)(frame_counter - delayed[i].frame) >= 0) { play_table(delayed[i].table_idx); delayed[i] = delayed[--ndelayed]; }
        else i++;
    }
    service_slots();
    if (music_open && music_stream) {
        float buf[4096]; float **pcm; int sec, ch = ov_info(&vf, -1)->channels;
        while (SDL_GetAudioStreamQueued(music_stream) < 44100 * 8 / 2) {   /* keep ~0.5 s queued (f32 stereo) */
            long n = ov_read_float(&vf, &pcm, (int)(sizeof buf / sizeof *buf) / ch, &sec);   /* unclipped: tracks peak > 0 dBFS */
            if (n <= 0) { if (n == 0 && music_loop) { ov_raw_seek(&vf, 0); continue; } break; }
            for (long i = 0; i < n; i++) for (int c = 0; c < ch; c++) buf[i * ch + c] = pcm[c][i];
            SDL_PutAudioStreamData(music_stream, buf, (int)(n * ch * sizeof(float)));
        }
    }
}

bool audio_init(void)
{
    crc_init();
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return false; }
    SDL_SetAudioPostmixCallback(dev, postmix, NULL);
    return true;
}

void audio_shutdown(void)
{
    music_stop();
    for (int v = 0; v < MAX_VOICES; v++) if (voices[v]) SDL_DestroyAudioStream(voices[v]);
    if (dev) SDL_CloseAudioDevice(dev);
}
