/* Saturn game audio (plan 7): the SH-2 decodes and mixes nothing.
 *
 * SFX and voices: celeriyacon's adp68k driver (third_party/scspadpcm, built into adp68k_bin.h by
 * `make -f Makefile.saturn adp68k`). The 68000 only programs slots; the SCSP DSP decodes 8 ADPCM channels at 44.1 kHz,
 * each from 4 slots reading a sample's block headers and data bytes straight out of sound RAM. The SH-2 talks to it
 * through the control block at 0x80 (per channel: action, sample id, left/right volume; the CD input's volumes) and a
 * software interrupt (SCIPD bit 5, which the 68000 clears when it has taken the block): aud_update sends what changed
 * once a frame.
 * Samples ("ADPK" blocks of SND.PCK, tools/saturn/build_disc.py bake_audio) are copied into the sound RAM bank when first
 * needed and stay there, the least recently used ones not playing making room when it is full; the effect table at
 * 0x2000 gives the driver a sample's address by id. Every sample is encoded looping at its start: the word at +2 (the
 * loop block) says whether it plays once (the block count: it then loops its silent tail) or loops (1).
 * Music: CD-DA through the DSP's CD input (cd_sat.c plays the tracks; MUSIC.TXT maps a music id to its track).
 * Movies: video_sat.c brackets a clip with aud_movie_begin / aud_movie_end. The 68000 is stopped and the film player
 * drives slots 0-1 over its ring at MOVIE_OFF itself (film_pcm_*); the bank below it survives, so afterwards only the
 * 8 KB driver is copied in again.
 *
 * Sound RAM: 0x00000 driver (control block at 0x80) | 0x02000 effect table | 0x02400 bank | 0x78000 movie ring(s). */
#include "../aud.h"
#include "../../pack.h"
#include "../../assets.h"
#include "sat_internal.h"
#include <yaul.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "adp68k_bin.h"

#define SND_RAM      0x25A00000u
#define SND16(off)   (*(volatile uint16_t *)(SND_RAM + (off)))
#define SCSP_REG(o)  (*(volatile uint16_t *)(0x25B00000u + (o)))
#define SLOT(n, r)   SCSP_REG((uint32_t)(n) * 0x20u + (r))   /* r: the register's byte offset */
#define SCSP_MVOL    SCSP_REG(0x400)
#define SCSP_SCIPD   SCSP_REG(0x420)
#define SCSP_SCIRE   SCSP_REG(0x422)
#define SCSP_MCIPD   SCSP_REG(0x42C)
#define SCSP_MCIRE   SCSP_REG(0x42E)
#define IRQ_DRIVER   0x0020u   /* SCIPD / MCIPD bit 5: the mailbox */
#define IRQ_SAMPLE   0x0400u   /* SCIPD bit 10: one sample (44.1 kHz) */

#define DRV_BYTES    0x2000u
#define CB_OFF       0x80u     /* adp68k.h SoundControlBlock: adpcm[8] (6 bytes each), psg, cd_volume[4] at +0x70 */
#define CD_VOL_OFF   (CB_OFF + 0x70u)
#define TABLE_OFF    0x2000u   /* adp68k_effect_table: a sample's sound RAM address by id */
#define TABLE_IDS    256
#define BANK_OFF     0x2400u
#define BANK_END     0x78000u
#define MOVIE_OFF    0x78000u  /* video_sat.c: the film player's ring(s), 2 x 16 KB */

#define ACT_PLAY     0x11u     /* adp68k.h ADP68K_ACTION_* */
#define ACT_STOP     0x01u
#define CHANNELS     8
#define RATE         44100u
#define MAX_SAMPLES  192
#define MAX_TRACKS   32

struct AudSample {
    uint32_t key;
    uint32_t addr, bytes, samples;   /* in the bank (addr 0: not there) */
    uint16_t blocks;                 /* the loop word when it plays once */
    uint32_t last_use;
    uint8_t id;
    bool missing, kept, looped;      /* looped: the loop word in sound RAM says loop */
    uint16_t users;                  /* channels playing it */
};

static AudSample samples[MAX_SAMPLES];
static int nsamples;
static uint8_t id_used[TABLE_IDS];
static uint32_t use_clock;

typedef struct {
    bool active, loop;
    AudSample *s;
    uint32_t end_us;                 /* a one-shot's end (sat_timer_us), give or take a frame */
    uint16_t vol, gen;
    uint8_t action;                  /* to send: 0, ACT_PLAY, ACT_STOP */
    bool vol_dirty;
} Voice;
static Voice voices[CHANNELS];

static struct { uint32_t id; int track; } tracks[MAX_TRACKS];
static int ntracks;
static uint16_t cd_vol = 0x4000;
static bool cd_dirty = true, driver_ok, movie;
static unsigned kicks_late;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t vol_of(real g) { int32_t v = (int32_t)g; return v <= 0 ? 0 : v >= FX_ONE ? 0x4000 : (uint16_t)(v >> 2); }   /* 16.16 -> 0x4000 = 1.0 */

/* ---- the driver ---- */
static void wait_samples(int n) { while (n--) { SCSP_SCIRE = IRQ_SAMPLE; for (int k = 0; k < 20000 && !(SCSP_SCIPD & IRQ_SAMPLE); k++) { } } }

/* the 68000 off and every slot, timer and DSP register cleared (the sound RAM is kept) */
static void scsp_quiet(void)
{
    smpc_smc_sndoff_call();
    SCSP_MVOL = 1u << 9;   /* 4 Mbit sound RAM, master volume 0 */
    for (int n = 0; n < 32; n++) { SLOT(n, 0x00) = 0; SLOT(n, 0x0A) = 0x001F; }
    SLOT(0, 0x00) = 1u << 12;   /* KYONEX: the key-offs take effect */
    wait_samples(256);
    for (int t = 0; t < 2; t++) {
        for (uint32_t o = 0x000; o < 0x400; o += 2) SCSP_REG(o) = 0;
        for (uint32_t o = 0x402; o < 0x1000; o += 2) SCSP_REG(o) = 0;
        wait_samples(256);
    }
    SCSP_SCIRE = 0xFFFF; SCSP_MCIRE = 0xFFFF;
}

static bool driver_start(void)
{
    smpc_smc_sndoff_call();
    SCSP_MVOL = 1u << 9;
    for (uint32_t i = 0; i < DRV_BYTES; i += 2) SND16(i) = (uint16_t)(adp68k_bin[i] << 8 | adp68k_bin[i + 1]);
    SCSP_MCIRE = 0xFFFF; SCSP_SCIRE = 0xFFFF;
    smpc_smc_sndon_call();
    uint32_t t0 = sat_timer_us();
    while (!(SCSP_MCIPD & IRQ_DRIVER))   /* the driver says it is up (it has set the SCSP up: ~20 ms) */
        if (sat_timer_us() - t0 > 500000u) { printf("snd: adp68k did not start\n"); return false; }
    SCSP_MCIRE = IRQ_DRIVER;
    for (int i = 0; i < CHANNELS; i++) { voices[i].active = false; voices[i].action = 0; voices[i].vol_dirty = true; }
    for (int i = 0; i < nsamples; i++) samples[i].users = 0;
    cd_dirty = true;
    return true;
}

/* what changed, to the driver, once it has taken the last one (its update takes ~14 samples, 0.32 ms) */
static void send(void)
{
    bool any = cd_dirty;
    for (int i = 0; i < CHANNELS; i++) any |= voices[i].action || voices[i].vol_dirty;
    if (!any || !driver_ok) return;
    uint32_t t0 = sat_timer_us();
    while (SCSP_SCIPD & IRQ_DRIVER)
        if (sat_timer_us() - t0 > 500u) { kicks_late++; return; }   /* the next step / frame sends it */
    for (int i = 0; i < CHANNELS; i++) {
        Voice *v = &voices[i]; uint32_t cb = CB_OFF + (uint32_t)i * 6u;
        if (v->vol_dirty) { SND16(cb + 2) = v->vol; SND16(cb + 4) = v->vol; v->vol_dirty = false; }
        if (v->action) { SND16(cb) = (uint16_t)(v->action << 8 | (v->action == ACT_PLAY && v->s ? v->s->id : 0)); v->action = 0; }
    }
    if (cd_dirty) { SND16(CD_VOL_OFF) = cd_vol; SND16(CD_VOL_OFF + 2) = 0; SND16(CD_VOL_OFF + 4) = 0; SND16(CD_VOL_OFF + 6) = cd_vol; cd_dirty = false; }
    SCSP_SCIPD = IRQ_DRIVER;
}

/* ---- the sample bank ---- */
static void sample_evict(AudSample *s)
{
    id_used[s->id] = 0;
    SND16(TABLE_OFF + s->id * 4u) = 0; SND16(TABLE_OFF + s->id * 4u + 2) = 0;
    s->addr = 0;
}

/* the lowest gap of n bytes in the bank, 0 if none */
static uint32_t bank_gap(uint32_t n)
{
    uint32_t at = BANK_OFF;
    for (;;) {
        bool clash = false;
        for (int i = 0; i < nsamples; i++) {
            const AudSample *s = &samples[i];
            if (s->addr && s->addr < at + n && s->addr + s->bytes > at) { clash = true; at = s->addr + s->bytes; }
        }
        if (!clash) return at + n <= BANK_END ? at : 0;
    }
}

static bool bank_alloc(uint32_t n, uint32_t *at)
{
    for (int pass = 0; pass < 2; pass++)   /* evicting samples that are not kept first, then kept ones (they come back when wanted) */
        for (;;) {
            if ((*at = bank_gap(n))) return true;
            AudSample *lru = NULL;
            for (int i = 0; i < nsamples; i++) {
                AudSample *s = &samples[i];
                if (s->addr && !s->users && (pass || !s->kept) && (!lru || s->last_use < lru->last_use)) lru = s;
            }
            if (!lru) break;
            sample_evict(lru);
        }
    return false;
}

static bool sample_load(AudSample *s)
{
    if (!s || s->missing) return false;
    s->last_use = ++use_clock;
    if (s->addr) return true;
    const PackEntry *e = packs_find_type(s->key, RES_SAMPLE);
    if (!e || e->size < 16 || memcmp(e->data, "ADPK", 4)) {
        printf("snd: sample %08lX missing/bad\n", (unsigned long)s->key);
        s->missing = true;
        return false;
    }
    const uint8_t *adp = e->data + 8;
    uint32_t n = (e->size - 8 + 1) & ~1u;
    int id = 0; while (id < TABLE_IDS && id_used[id]) id++;
    uint32_t at;
    if (id == TABLE_IDS || !bank_alloc(n, &at)) {
        printf("snd: no room for %08lX (%lu bytes)\n", (unsigned long)s->key, (unsigned long)n);
        packs_release_type(s->key, RES_SAMPLE);
        return false;
    }
    s->samples = rd32le(e->data + 4); s->blocks = (uint16_t)(adp[0] << 8 | adp[1]); s->bytes = n;
    /* sound RAM takes 16-bit writes from the SH-2: the .adp bytes in big-endian pairs, as the 68000 reads them */
    for (uint32_t i = 0; i < n; i += 2) {
        uint8_t a = adp[i], b = i + 1 < e->size - 8 ? adp[i + 1] : 0;
        SND16(at + i) = (uint16_t)(a << 8 | b);
    }
    SND16(at + 2) = s->blocks; s->looped = false;   /* plays once until asked to loop */
    packs_release_type(s->key, RES_SAMPLE);
    s->addr = at; s->id = (uint8_t)id; id_used[id] = 1;
    SND16(TABLE_OFF + id * 4u) = (uint16_t)(at >> 16); SND16(TABLE_OFF + id * 4u + 2) = (uint16_t)at;
    return true;
}

static AudSample *sample_find(uint32_t key)
{
    for (int i = 0; i < nsamples; i++) if (samples[i].key == key) return samples[i].missing ? NULL : &samples[i];
    if (nsamples >= MAX_SAMPLES) return NULL;
    AudSample *s = &samples[nsamples++]; memset(s, 0, sizeof *s); s->key = key;
    return s;
}

AudSample *aud_sample_pack(uint32_t id) { return sample_find(id); }
AudSample *aud_sample_file(const char *path) { return path ? sample_find(asset_key(path)) : NULL; }
void aud_prefetch(AudSample *s) { (void)sample_load(s); }
void aud_keep(AudSample *s, bool loop) { (void)loop; if (s && sample_load(s)) s->kept = true; }

/* ---- voices ---- */
static void voice_release(Voice *v)
{
    if (!v->active) return;
    if (v->s && v->s->users) v->s->users--;
    v->active = false; v->s = NULL;
}

int aud_play(AudSample *s, real gain, bool loop)
{
    if (movie || !sample_load(s)) return -1;
    int best = -1;
    for (int i = 0; i < CHANNELS && best < 0; i++) if (!voices[i].active) best = i;
    for (int pass = 0; pass < 2 && best < 0; pass++)   /* all busy: the one-shot nearest its end, then any loop */
        for (int i = 0; i < CHANNELS; i++)
            if ((pass || !voices[i].loop) && (best < 0 || (int32_t)(voices[i].end_us - voices[best].end_us) < 0)) best = i;
    Voice *v = &voices[best];
    voice_release(v);
    if (s->looped != loop) { SND16(s->addr + 2) = loop ? 1 : s->blocks; s->looped = loop; }
    v->active = true; v->loop = loop; v->s = s; s->users++;
    /* it starts at the next send (this frame), after the driver's one block of lead-in */
    v->end_us = sat_timer_us() + 17000u + (uint32_t)(((uint64_t)(s->samples + 16u) * 1000000u) / RATE);
    uint16_t vol = vol_of(gain); if (vol != v->vol) { v->vol = vol; v->vol_dirty = true; }
    v->action = ACT_PLAY;
    v->gen++; if (!v->gen) v->gen = 1;
    return best | ((int)v->gen << 8);
}

static Voice *voice_handle(int h) { int i = h & 0xff; return h >= 0 && i < CHANNELS && voices[i].active && ((int)voices[i].gen << 8 | i) == h ? &voices[i] : NULL; }
void aud_set_gain(int h, real gain) { Voice *v = voice_handle(h); if (!v) return; uint16_t vol = vol_of(gain); if (vol != v->vol) { v->vol = vol; v->vol_dirty = true; } }
void aud_stop(int h) { Voice *v = voice_handle(h); if (!v) return; voice_release(v); v->action = ACT_STOP; }
bool aud_playing(int h) { return voice_handle(h) != NULL; }

/* ---- music: CD-DA ---- */
static int track_of(uint32_t id) { for (int i = 0; i < ntracks; i++) if (tracks[i].id == id) return tracks[i].track; return 0; }

bool aud_music_play(uint32_t id, bool loop)
{
    int t = track_of(id);
    if (!t) { printf("music: %08lX has no track\n", (unsigned long)id); return false; }
    return cd_sat_cdda_play(t, loop);
}
void aud_music_stop(void) { cd_sat_cdda_stop(); }
void aud_music_gain(real g) { uint16_t v = vol_of(g); if (v != cd_vol) { cd_vol = v; cd_dirty = true; } }
void aud_music_pause(bool pause) { cd_sat_cdda_pause(pause); }

/* ---- movies (video_sat.c) ---- */
void aud_movie_begin(void)
{
    if (movie) return;
    for (int i = 0; i < CHANNELS; i++) { voice_release(&voices[i]); voices[i].action = 0; }
    scsp_quiet();
    SCSP_MVOL = 1u << 9 | 0xF;
    movie = true; driver_ok = false;
}

void aud_movie_end(void)
{
    if (!movie) return;
    movie = false;
    driver_ok = driver_start();
}

void aud_clock_change(bool begin)
{
    if (movie) return;
    if (begin) {   /* what plays is cut (the SCSP is reset): nothing is left counted as playing */
        for (int i = 0; i < CHANNELS; i++) { voice_release(&voices[i]); voices[i].action = 0; }
        driver_ok = false;
    } else driver_ok = driver_start();
}

/* the film player's slots (third_party/libyaul_cinepak/film_snd.c): slot ch loops over bytes of 8/16-bit PCM at off */
static uint16_t pitch(uint32_t rate)   /* OCT / FNS: rate = 44100 x 2^oct x (1024 + fns) / 1024 */
{
    int oct = 0;
    while (rate < 44100u && oct > -8) { rate <<= 1; oct--; }
    while (rate >= 88200u && oct < 7) { rate >>= 1; oct++; }
    return (uint16_t)(((unsigned)oct & 0xF) << 11 | ((rate * 1024u / 44100u - 1024u) & 0x3FF));
}

static uint16_t film_slot0[2], film_pan[2];
void film_pcm_configure(int ch, uint32_t off, uint32_t bytes, uint16_t rate, uint8_t bits, uint8_t pan)
{
    if (!movie || ch < 0 || ch > 1) return;   /* the slots are the driver's outside a clip */
    film_slot0[ch] = (uint16_t)(1u << 5 | (bits == 8 ? 1u << 4 : 0) | ((off >> 16) & 0xF));   /* LPCTL normal loop, PCM8B */
    film_pan[ch] = pan & 0x1F;
    SLOT(ch, 0x00) = film_slot0[ch];
    SLOT(ch, 0x02) = (uint16_t)off;                              /* SA */
    SLOT(ch, 0x04) = 0;                                          /* LSA */
    SLOT(ch, 0x06) = (uint16_t)(bits == 8 ? bytes : bytes / 2);  /* LEA, in samples */
    SLOT(ch, 0x08) = 0x001F;                                     /* AR 31: full level at once */
    SLOT(ch, 0x0A) = (uint16_t)(0xF << 10 | 0x1F);               /* KRS off, RR 31 */
    SLOT(ch, 0x0C) = 0; SLOT(ch, 0x0E) = 0;
    SLOT(ch, 0x10) = pitch(rate);
    SLOT(ch, 0x12) = 0; SLOT(ch, 0x14) = 0; SLOT(ch, 0x16) = 0;
}
void film_pcm_start(int ch, uint8_t volume)
{
    if (!movie || ch < 0 || ch > 1) return;   /* the slots are the driver's outside a clip */
    SLOT(ch, 0x16) = (uint16_t)((volume & 7) << 13 | film_pan[ch] << 8);   /* direct send */
    SLOT(ch, 0x00) = (uint16_t)(film_slot0[ch] | 1u << 11 | 1u << 12);   /* KYONB + KYONEX */
}
void film_pcm_stop(int ch)
{
    if (!movie || ch < 0 || ch > 1) return;   /* the slots are the driver's outside a clip */
    SLOT(ch, 0x00) = (uint16_t)((film_slot0[ch] & ~(1u << 11)) | 1u << 12);
}

/* ---- once a frame ---- */
void aud_update(void)
{
    if (movie) return;   /* the clip reads the disc: the music comes back after it */
    cd_sat_cdda_update();
    uint32_t now = sat_timer_us();
    for (int i = 0; i < CHANNELS; i++) {
        Voice *v = &voices[i];
        if (v->active && !v->loop && (int32_t)(now - v->end_us) >= 0) voice_release(v);   /* the channel plays its silent tail */
    }
    send();
}

bool aud_init(void)
{
    FILE *f = fopen("MUSIC.TXT", "r");
    if (f) {
        unsigned id; int t;
        while (ntracks < MAX_TRACKS && fscanf(f, "%x %d", &id, &t) == 2) { tracks[ntracks].id = id; tracks[ntracks].track = t; ntracks++; }
        fclose(f);
    }
    cd_sat_stream_stop();
    for (uint32_t i = TABLE_OFF; i < BANK_OFF; i += 2) SND16(i) = 0;
    driver_ok = driver_start();
    printf("snd: adp68k (8 ADPCM channels on the SCSP DSP), %lu KB bank, %d CD-DA tracks%s\n",
           (unsigned long)((BANK_END - BANK_OFF) / 1024), ntracks, driver_ok ? "" : ", NO DRIVER");
    return driver_ok;
}

void aud_shutdown(void)
{
    aud_music_stop();
    for (int i = 0; i < CHANNELS; i++) if (voices[i].active) { voice_release(&voices[i]); voices[i].action = ACT_STOP; }
    send();
    unsigned kb = 0; for (int i = 0; i < nsamples; i++) if (samples[i].addr) kb += samples[i].bytes;
    printf("snd: %u KB of samples resident, %u late sends\n", kb / 1024, kicks_late);
}
