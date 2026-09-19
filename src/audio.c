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
static SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };

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

static void play_sfx(Sfx *s)
{
    if (!dev || !s || !s->pcm) return;
    SDL_AudioSpec in = { SDL_AUDIO_S16, s->channels, 44100 };
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v] && SDL_GetAudioStreamQueued(voices[v]) > 0) continue;
        if (voices[v]) SDL_DestroyAudioStream(voices[v]);
        voices[v] = SDL_CreateAudioStream(&in, &spec);
        if (!voices[v]) return;
        SDL_BindAudioStream(dev, voices[v]);
        SDL_PutAudioStreamData(voices[v], s->pcm, s->frames * s->channels * 2);
        SDL_FlushAudioStream(voices[v]);
        return;
    }
}

#define MAX_EXTRA 16
static struct { uint32_t id; Sfx s; } extra[MAX_EXTRA]; static int nextra;
void sfx_play_id(uint32_t id)
{
    for (int i = 0; i < nextra; i++) if (extra[i].id == id) { play_sfx(&extra[i].s); return; }
    if (nextra == MAX_EXTRA) return;
    extra[nextra].id = id; memset(&extra[nextra].s, 0, sizeof(Sfx));
    if (load_sfx_entry(packs_find(id), &extra[nextra].s)) play_sfx(&extra[nextra].s);
    nextra++;
}

static void play_table(int idx)
{
    if (idx < 0 || idx >= 32) return;
    play_sfx(load_sfx(idx));
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
    SDL_AudioSpec in = { SDL_AUDIO_S16, vi->channels, (int)vi->rate };
    music_stream = SDL_CreateAudioStream(&in, &spec);
    SDL_BindAudioStream(dev, music_stream);
    audio_update();
}

void audio_update(void)
{
    frame_counter++;
    for (int i = 0; i < ndelayed; ) {
        if ((int)(frame_counter - delayed[i].frame) >= 0) { play_table(delayed[i].table_idx); delayed[i] = delayed[--ndelayed]; }
        else i++;
    }
    if (music_open && music_stream) {
        char buf[8192]; int sec;
        while (SDL_GetAudioStreamQueued(music_stream) < 44100 * 4 / 2) {   /* keep ~0.5 s queued */
            long n = ov_read(&vf, buf, sizeof buf, 0, 2, 1, &sec);
            if (n <= 0) { if (n == 0 && music_loop) { ov_raw_seek(&vf, 0); continue; } break; }
            SDL_PutAudioStreamData(music_stream, buf, (int)n);
        }
    }
}

bool audio_init(void)
{
    crc_init();
    dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return false; }
    return true;
}

void audio_shutdown(void)
{
    music_stop();
    for (int v = 0; v < MAX_VOICES; v++) if (voices[v]) SDL_DestroyAudioStream(voices[v]);
    if (dev) SDL_CloseAudioDevice(dev);
}
