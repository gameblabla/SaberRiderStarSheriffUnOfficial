/* Saturn game audio (plan 7): the SH-2 decodes and mixes nothing.
 *
 * SFX and voices: celeriyacon's adp68k driver (third_party/scspadpcm, built into adp68k_bin.h by
 * `make -f Makefile.saturn adp68k`). The 68000 only programs slots; the SCSP DSP decodes 8 ADPCM channels at 44.1 kHz,
 * each from 4 slots reading a sample's block headers and data bytes straight out of sound RAM. The SH-2 talks to it
 * through the control block at 0x80 (per channel: action, sample id, left/right volume; the CD input's volumes) and a
 * software interrupt (SCIPD bit 5, which the 68000 clears when it has taken the block): aud_update sends what changed
 * once a frame.
 * Samples ("ADPK" blocks of SND.PCK, tools/saturn/build_disc.py bake_audio) are copied into the sound RAM bank when first
 * preparing a scene and stay there until the next scene. Playback never reads the disc. The effect table at
 * 0x2000 gives the driver a sample's address by id. Every sample is encoded looping at its start: the word at +2 (the
 * loop block) says whether it plays once (the block count: it then loops its silent tail) or loops (1).
 * Music: CD-DA through the DSP's CD input (cd_sat.c plays the tracks; MUSIC.TXT maps a music id to its track).
 * Movies: video_sat.c brackets a clip with aud_movie_begin / aud_movie_end. The 68000 runs an idle program and the film
 * player drives slots 0-1 over two 64 KB PCM rings above the resident sample bank (film_pcm_*). Resident samples stay
 * in place, so afterwards only the 8 KB driver is copied in again. A clip played from RAM (the briefing)
 * leaves the drive to the music: the CD input then goes straight to the output (slots 16 / 17's EFSDL, the DSP is off)
 * and the music plays on under it.
 *
 * Sound RAM: 0x00000 driver (control block at 0x80) | 0x02000 effect table | 0x02400 bank (up to 0x60000) | two
 * 64 KB movie PCM rings at 0x60000. */
#include "../aud.h"
#include "../plat.h"
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
#define BANK_END     SAT_MOVIE_PCM_BASE
#define MOVIE_OFF    SAT_MOVIE_PCM_BASE  /* video_sat.c: two 64 KB PCM rings */

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
    uint32_t scene_mask;             /* SOUNDS.BIN: bit (stage * 4 + hero) */
    uint8_t id;
    bool missing, looped, reported; /* looped: the loop word in sound RAM says loop */
    uint16_t users;                  /* logical voices, queued mailboxes and acknowledged hardware channels */
};

static AudSample samples[MAX_SAMPLES];
static int nsamples;
static uint8_t id_used[TABLE_IDS];
static bool bank_loading;
static uint32_t bank_next;

typedef struct {
    bool active, loop, started;
    AudSample *s;
    AudSample *hw_s, *pending_s;    /* refs held until the 68000 acknowledges stop/replace commands */
    uint32_t end_us;                 /* a one-shot's end (sat_timer_us), give or take a frame */
    uint16_t vol, gen, pending_gen;
    uint8_t action;                  /* to send: 0, ACT_PLAY, ACT_STOP */
    uint8_t pending_action;
    bool vol_dirty;
} Voice;
static Voice voices[CHANNELS];

static struct { uint32_t id; int track; uint32_t sectors; } tracks[MAX_TRACKS];
static int ntracks;
static uint16_t cd_vol = 0x4000;
static bool cd_dirty = true, driver_ok, movie, movie_music;
static unsigned kicks_late;
static int test_sample, test_pass;
static uint32_t test_at;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t vol_of(real g) { int32_t v = (int32_t)g; return v <= 0 ? 0 : v >= FX_ONE ? 0x4000 : (uint16_t)(v >> 2); }   /* 16.16 -> 0x4000 = 1.0 */
static void sample_ref(AudSample *s);
static void sample_unref(AudSample *s);

/* ---- the driver ---- */
static void wait_samples(int n) { while (n--) { SCSP_SCIRE = IRQ_SAMPLE; for (int k = 0; k < 20000 && !(SCSP_SCIPD & IRQ_SAMPLE); k++) { } } }

/* the 68000 off / on (SNDOFF / SNDON: never over the vblank's INTBACK, sat_smpc_lock) */
static void snd_cpu(bool on)
{
    sat_smpc_lock();
    if (on) smpc_smc_sndon_call(); else smpc_smc_sndoff_call();
    sat_smpc_unlock();
}

/* Zero sound RAM [from, to) with word stores (the SH-2 window does not take byte stores reliably). Sound RAM powers up
 * with whatever the console's DRAM holds (it differs between hardware revisions; an emulator starts it at zero), and
 * nothing the driver reads may depend on it: Sega Technical Bulletin #36 - initialize every chip, never rely on the
 * state the boot ROM or power-on left. */
static void sound_ram_zero(uint32_t from, uint32_t to)
{
    for (uint32_t o = from; o < to; o += 2) SND16(o) = 0;
}

/* the 68000 off and every slot, timer and DSP register cleared (the sound RAM is kept) */
static void scsp_quiet(void)
{
    snd_cpu(false);
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

/* Sega Technical Bulletin #51: SNDOFF is only for loading a program; the
 * sound CPU must run even when the SH-2 owns the SCSP. Keep it fetching an
 * idle loop throughout a movie instead of holding it in reset for seconds.
 * Only the driver's area is replaced; the resident sample bank survives.
 * Reset starts with SR=$2700, so this program needs no interrupt vectors. */
static void movie_cpu_start(void)
{
    SND16(0) = 0; SND16(2) = 0x80;   /* initial SSP */
    SND16(4) = 0; SND16(6) = 8;      /* initial PC */
    SND16(8) = 0x60FE;               /* BRA.S to itself */
    snd_cpu(true);
}

/* quiet: the movie / clock-change restarts come with the SCSP in whatever state the clip or the BIOS left it (the film
 * player's slots 0-1 and its timer C, the CD pass-through on slots 16 / 17, a running DSP): reset all of it first, as
 * the boot does, so the driver starts on a silent chip. */
static bool driver_start(bool quiet)
{
    if (quiet) scsp_quiet(); else snd_cpu(false);   /* both leave the 68000 off */
    SCSP_MVOL = 1u << 9;
    for (uint32_t i = 0; i < DRV_BYTES; i += 2) SND16(i) = (uint16_t)(adp68k_bin[i] << 8 | adp68k_bin[i + 1]);
    SCSP_MCIRE = 0xFFFF; SCSP_SCIRE = 0xFFFF;
    snd_cpu(true);
    uint32_t t0 = sat_timer_us();
    while (!(SCSP_MCIPD & IRQ_DRIVER))   /* the driver says it is up (it has set the SCSP up: ~20 ms) */
        if (sat_timer_us() - t0 > 500000u) { printf("snd: adp68k did not start\n"); return false; }
    SCSP_MCIRE = IRQ_DRIVER;
    for (int i = 0; i < CHANNELS; i++) {
        voices[i].active = false; voices[i].s = voices[i].hw_s = voices[i].pending_s = NULL;
        voices[i].action = voices[i].pending_action = 0; voices[i].vol_dirty = true;
    }
    for (int i = 0; i < nsamples; i++) samples[i].users = 0;
    cd_dirty = true;
    return true;
}

/* The driver's mailbox bit goes low only after the 68000 has consumed the previous channel commands. Both samples in a
 * replacement stay pinned until then, so bank allocation cannot overwrite memory still read by the DSP or mailbox. */
static void driver_ack(void)
{
    if (SCSP_SCIPD & IRQ_DRIVER) return;
    for (int i = 0; i < CHANNELS; i++) {
        Voice *v = &voices[i];
        if (!v->pending_action) continue;
        sample_unref(v->hw_s);
        v->hw_s = v->pending_s;
        v->pending_s = NULL;       /* transfer the queued pin to the hardware-owned reference */
        if (v->pending_action == ACT_PLAY && v->active && v->gen == v->pending_gen) {
            v->started = true;
            v->end_us = sat_timer_us() + 17000u + (uint32_t)(((uint64_t)(v->s->samples + 16u) * 1000000u) / RATE);
        }
        v->pending_action = 0;
    }
}

/* what changed, to the driver, once it has taken the last one (its update takes ~14 samples, 0.32 ms) */
static void send(void)
{
    driver_ack();
    bool any = cd_dirty;
    for (int i = 0; i < CHANNELS; i++) any |= voices[i].action || voices[i].vol_dirty;
    if (!any || !driver_ok) return;
    uint32_t t0 = sat_timer_us();
    while (SCSP_SCIPD & IRQ_DRIVER)
        if (sat_timer_us() - t0 > 500u) { kicks_late++; return; }   /* the next step / frame sends it */
    driver_ack();
    for (int i = 0; i < CHANNELS; i++) {
        Voice *v = &voices[i]; uint32_t cb = CB_OFF + (uint32_t)i * 6u;
        if (v->vol_dirty) { SND16(cb + 2) = v->vol; SND16(cb + 4) = v->vol; v->vol_dirty = false; }
        if (v->action) {
            uint8_t action = v->action;
            AudSample *next = action == ACT_PLAY ? v->s : NULL;
            sample_ref(next);
            v->pending_s = next; v->pending_action = action;
            v->pending_gen = v->gen;
            SND16(cb) = (uint16_t)(action << 8 | (next ? next->id : 0));
            v->action = 0;
        }
    }
    if (cd_dirty) { SND16(CD_VOL_OFF) = cd_vol; SND16(CD_VOL_OFF + 2) = 0; SND16(CD_VOL_OFF + 4) = 0; SND16(CD_VOL_OFF + 6) = cd_vol; cd_dirty = false; }
    SCSP_SCIPD = IRQ_DRIVER;
}

/* ---- the sample bank ---- */
static void sample_ref(AudSample *s)
{
    if (s && s->users != UINT16_MAX) s->users++;
}

static void sample_unref(AudSample *s)
{
    if (!s || !s->users) return;
    s->users--;
}

static bool sample_load(AudSample *s)
{
    if (!s || s->missing) return false;
    if (s->addr) return true;
    if (!bank_loading) {
        if (!s->reported) printf("snd: NONRESIDENT %08lX (disc read forbidden)\n", (unsigned long)s->key);
        s->reported = true;
        return false;
    }
    const PackEntry *e = packs_find_type(s->key, RES_SAMPLE);
    if (!e || e->size < 16 || memcmp(e->data, "ADPK", 4)) {
        if (e) printf("snd: sample %08lX bad: %u bytes at %p, %02X %02X %02X %02X %02X %02X %02X %02X\n", (unsigned long)s->key,
                      (unsigned)e->size, (const void *)e->data, e->data[0], e->data[1], e->data[2], e->data[3], e->data[4], e->data[5], e->data[6], e->data[7]);
        printf("snd: sample %08lX missing/bad\n", (unsigned long)s->key);
        s->missing = true;
        return false;
    }
    const uint8_t *adp = e->data + 8;
    uint32_t n = (e->size - 8 + 1) & ~1u;
    int id = 0; while (id < TABLE_IDS && id_used[id]) id++;
    uint32_t at = bank_next;
    if (n != s->bytes || id == TABLE_IDS || n > BANK_END - at) {
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
    bank_next += n;
    SND16(TABLE_OFF + id * 4u) = (uint16_t)(at >> 16); SND16(TABLE_OFF + id * 4u + 2) = (uint16_t)at;
    return true;
}

static AudSample *sample_find(uint32_t key)
{
    for (int i = 0; i < nsamples; i++) if (samples[i].key == key) return samples[i].missing ? NULL : &samples[i];
    printf("snd: unknown sample %08lX\n", (unsigned long)key);
    return NULL;
}

AudSample *aud_sample_pack(uint32_t id) { return sample_find(id); }
AudSample *aud_sample_file(const char *path) { return path ? sample_find(asset_key(path)) : NULL; }
void aud_prefetch(AudSample *s) { (void)sample_load(s); }
void aud_keep(AudSample *s, bool loop) { (void)loop; (void)sample_load(s); }
void aud_unkeep(AudSample *s) { (void)s; } /* scene ownership outlives hero override handles */

/* ---- voices ---- */
static void voice_release(Voice *v)
{
    if (!v->active) return;
    sample_unref(v->s);
    v->active = false; v->s = NULL;
}

static struct { AudSample *s; real gain; uint32_t us; } pend;   /* a one-shot asked for during a clip */

int aud_play(AudSample *s, real gain, bool loop)
{
    if (movie) { if (!loop) { pend.s = s; pend.gain = gain; pend.us = sat_timer_us(); } return -1; }
    if (!driver_ok || !sample_load(s)) return -1;
    int best = -1;
    for (int i = 0; i < CHANNELS && best < 0; i++) if (!voices[i].active) best = i;
    for (int pass = 0; pass < 2 && best < 0; pass++)   /* all busy: the one-shot nearest its end, then any loop */
        for (int i = 0; i < CHANNELS; i++)
            if ((pass || !voices[i].loop) && (best < 0 || (int32_t)(voices[i].end_us - voices[best].end_us) < 0)) best = i;
    Voice *v = &voices[best];
    voice_release(v);
    if (s->looped != loop) { SND16(s->addr + 2) = loop ? 1 : s->blocks; s->looped = loop; }
    v->active = true; v->loop = loop; v->started = false; v->s = s; sample_ref(s);
    v->end_us = 0;                  /* duration starts only after the driver acknowledges ACT_PLAY */
    uint16_t vol = vol_of(gain); if (vol != v->vol) { v->vol = vol; v->vol_dirty = true; }
    v->action = ACT_PLAY;
    v->gen++; if (!v->gen) v->gen = 1;
    return best | ((int)v->gen << 8);
}

static Voice *voice_handle(int h) { int i = h & 0xff; return h >= 0 && i < CHANNELS && voices[i].active && ((int)voices[i].gen << 8 | i) == h ? &voices[i] : NULL; }
void aud_set_gain(int h, real gain) { Voice *v = voice_handle(h); if (!v) return; uint16_t vol = vol_of(gain); if (vol != v->vol) { v->vol = vol; v->vol_dirty = true; } }
void aud_stop(int h) { Voice *v = voice_handle(h); if (!v) return; voice_release(v); v->action = ACT_STOP; }
bool aud_playing(int h) { return voice_handle(h) != NULL; }

/* Called only at a loading boundary. Stop and acknowledge EVERY hardware channel before
 * reusing the bank, including one-shots which already released their logical handles. */
bool aud_prepare_scene(int stage, int hero)
{
    /* Every refusal below is loud: the caller (level_start) cannot start a stage without a resident bank, and a silent
     * false here is a stage that never begins - no music, no effects, and a menu that will not respond. */
    if (movie) { printf("snd: scene %d hero %d refused: a clip owns the driver\n", stage, hero); return false; }
    if (!driver_ok) { printf("snd: scene %d hero %d refused: the driver is not up\n", stage, hero); return false; }
    if (stage < 0 || stage > 7 || hero < 0 || hero > 3) { printf("snd: scene %d hero %d refused: out of range\n", stage, hero); return false; }
    uint32_t mask = 1u << (stage * 4 + hero), total = 0;
    for (int i = 0; i < nsamples; i++) if (samples[i].scene_mask & mask) {
        const PackEntry *e = packs_peek_type(samples[i].key, RES_SAMPLE);
        if (!e || e->declen < 16 || ((e->declen - 8 + 1) & ~1u) != samples[i].bytes) {
            printf("snd: sound manifest/pack mismatch %08lX\n", (unsigned long)samples[i].key);
            return false;
        }
        if (samples[i].bytes > BANK_END - BANK_OFF - total) {
            printf("snd: scene %d hero %d refused: %08lX does not fit (%lu used, %lu bytes left)\n", stage, hero,
                   (unsigned long)samples[i].key, (unsigned long)total, (unsigned long)(BANK_END - BANK_OFF - total));
            return false;
        }
        total += samples[i].bytes;
    }
    if (!total) { printf("snd: scene %d hero %d refused: no sample is in this scene\n", stage, hero); return false; }
    aud_music_stop();
    pend.s = NULL;
    for (int i = 0; i < CHANNELS; i++) { voice_release(&voices[i]); voices[i].action = ACT_STOP; }
    uint32_t t0 = sat_timer_us();
    for (;;) {
        send();
        bool busy = false;
        for (int i = 0; i < CHANNELS; i++) busy |= voices[i].action || voices[i].pending_action;
        if (!busy) break;
        if (sat_timer_us() - t0 > 100000u) { printf("snd: bank stop acknowledgement timed out\n"); return false; }
    }
    for (int i = 0; i < nsamples; i++) {
        /* A sample still referenced by a voice cannot be thrown out of the bank: the 68000 may still be reading it.
         * The stops above were acknowledged, so this means a logical handle outlived its voice - a bug, and one that
         * used to strand the game in a menu with no explanation. */
        if (samples[i].users) {
            printf("snd: scene %d hero %d refused: %08lX still has %u user(s)\n", stage, hero,
                   (unsigned long)samples[i].key, samples[i].users);
            return false;
        }
        samples[i].addr = 0; samples[i].reported = false;
    }
    memset(id_used, 0, sizeof id_used);
    /* Every hardware channel is stopped and acknowledged: no slot reads the bank. Clear all of it, so what lies between
     * and after the scene's samples (the previous scene's, or a power-on pattern) is silence for a slot that reads
     * a few bytes past a sample's end, and the effect table has no stale entry. */
    sound_ram_zero(TABLE_OFF, BANK_END);
    bank_next = BANK_OFF; bank_loading = true;
    bool ok = true;
    for (int i = 0; i < nsamples; i++)
        if ((samples[i].scene_mask & mask) && !sample_load(&samples[i])) { ok = false; break; }
    bank_loading = false;
    test_sample = test_pass = 0; test_at = 0;
    printf("snd: scene %d hero %d %s: %lu/%lu bytes resident, %lu free\n", stage, hero, ok ? "READY" : "FAILED",
           (unsigned long)(bank_next - BANK_OFF), (unsigned long)(BANK_END - BANK_OFF), (unsigned long)(BANK_END - bank_next));
    return ok;
}

/* ---- music: CD-DA ---- */
static void movie_cd_level(void);
static int track_of(uint32_t id) { for (int i = 0; i < ntracks; i++) if (tracks[i].id == id) return tracks[i].track; return 0; }

bool aud_music_play(uint32_t id, bool loop)
{
    int t = track_of(id);
    if (!t) { printf("music: %08lX has no track\n", (unsigned long)id); return false; }
    return cd_sat_cdda_play(t, loop);
}
void aud_music_stop(void) { cd_sat_cdda_stop(); }
void aud_music_gain(real g) { uint16_t v = vol_of(g); if (v != cd_vol) { cd_vol = v; cd_dirty = true; if (movie) movie_cd_level(); } }
void aud_music_pause(bool pause) { cd_sat_cdda_pause(pause); }

/* ---- movies (video_sat.c) ---- */
/* the CD input straight to the output while the DSP is off: EFSDL of slots 16 (EXTS0, left) / 17 (EXTS1, right) in
 * 6 dB steps, the nearest to the driver's level: 7 (0 dB) for its 0x4000 (its DSP path measured in mednafen: the same
 * passage of the briefing track at ~0.7 of the track's RMS through the driver, ~0.2 at EFSDL 5) */
static void movie_cd_level(void)
{
    int l = 0;
    if (movie_music && cd_vol) { l = 7; for (uint32_t v = cd_vol; v * 181u / 128u < 0x4000u && l > 0; v <<= 1) l--; }
    SLOT(16, 0x16) = (uint16_t)(l << 5 | 0x1F);
    SLOT(17, 0x16) = (uint16_t)(l << 5 | 0x0F);
}

void aud_movie_begin(bool music)
{
    if (movie) return;
    for (int i = 0; i < CHANNELS; i++) { voice_release(&voices[i]); voices[i].action = 0; }
    scsp_quiet();
    movie_cpu_start();   /* restart immediately after loading, before movie bookkeeping */
    for (int i = 0; i < CHANNELS; i++) {
        voices[i].hw_s = voices[i].pending_s = NULL;
        voices[i].pending_action = 0;
    }
    for (int i = 0; i < nsamples; i++) samples[i].users = 0;
    SCSP_MVOL = 1u << 9 | 0xF;
    movie = true; movie_music = music; driver_ok = false;
    movie_cd_level();
}

void aud_movie_end(void)
{
    if (!movie) return;
    movie = false; movie_music = false;
    movie_cd_level();
    sound_ram_zero(MOVIE_OFF, 0x80000u);   /* the clip's ring(s): none of its audio is left to be read */
    driver_ok = driver_start(true);
    cd_dirty = true;
    if (pend.s && sat_timer_us() - pend.us < 200000u) aud_play(pend.s, pend.gain, false);   /* the click that closed it */
    pend.s = NULL;
}

void aud_clock_change(bool begin)
{
    if (movie) return;
    if (begin) {   /* what plays is cut (the SCSP is reset): nothing is left counted as playing */
        for (int i = 0; i < CHANNELS; i++) { voice_release(&voices[i]); voices[i].action = 0; }
        driver_ok = false;
    } else driver_ok = driver_start(true);
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
/* Opt-in emulator/hardware diagnostic: exercise every resident sound twice while music
 * runs, then eight simultaneous voices. Logs permit checking that no play caused a read. */
static void sound_test(uint32_t now)
{
    static int enabled = -1;
    if (enabled < 0) enabled = plat_getenv("SABER_SNDTEST") != NULL;
    if (!enabled || test_pass >= 3) return;
    if (!test_at) { test_at = now + 4000000u; return; }
    if ((int32_t)(now - test_at) < 0) return;
    while (test_sample < nsamples && !samples[test_sample].addr) test_sample++;
    if (test_sample == nsamples) {
        test_sample = 0; test_pass++;
        if (test_pass == 3) { printf("sndtest: DONE\n"); return; }
        while (test_sample < nsamples && !samples[test_sample].addr) test_sample++;
    }
    AudSample *s = &samples[test_sample++];
    int h = -1;
    for (int i = 0; i < (test_pass == 2 ? CHANNELS : 1); i++) h = aud_play(s, R(0.15f), false);
    printf("sndtest: pass %d sample %08lX handle %d at %lu ms\n", test_pass, (unsigned long)s->key, h, (unsigned long)(now / 1000u));
    test_at = now + 500000u;
}

void aud_update(void)
{
    if (movie && !movie_music) return;   /* the clip reads the disc: the music comes back after it */
    cd_sat_cdda_update();
    if (movie) return;
    driver_ack();
    uint32_t now = sat_timer_us();
    sound_test(now);
    for (int i = 0; i < CHANNELS; i++) {
        Voice *v = &voices[i];
        if (v->active && v->started && !v->loop && (int32_t)(now - v->end_us) >= 0) {
            voice_release(v);
            v->action = ACT_STOP;
        }
    }
    send();
}

bool aud_init(void)
{
    FILE *list = fopen("SOUNDS.BIN", "rb");
    uint8_t header[8];
    if (!list) { printf("snd: missing SOUNDS.BIN; rebuild the Saturn disc\n"); return false; }
    bool valid = fread(header, 1, sizeof header, list) == sizeof header && !memcmp(header, "SBN1", 4);
    uint32_t count = valid ? rd32le(header + 4) : 0;
    if (!count || count > MAX_SAMPLES) { fclose(list); return false; }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t row[12];
        if (fread(row, 1, sizeof row, list) != sizeof row) { fclose(list); return false; }
        samples[i].key = rd32le(row); samples[i].bytes = rd32le(row + 4); samples[i].scene_mask = rd32le(row + 8);
    }
    nsamples = (int)count;
    fclose(list);
    FILE *f = fopen("MUSIC.TXT", "r");
    if (f) {
        char line[96];
        while (ntracks < MAX_TRACKS && fgets(line, sizeof line, f)) {
            unsigned id, sectors = 0; int t;
            int fields = sscanf(line, "%x %d %u", &id, &t, &sectors);
            if (fields < 2) continue;
            tracks[ntracks].id = id; tracks[ntracks].track = t; tracks[ntracks].sectors = fields == 3 ? sectors : 0;
            if (tracks[ntracks].sectors) cd_sat_cdda_track_length(t, tracks[ntracks].sectors);
            ntracks++;
        }
        fclose(f);
    }
    cd_sat_stream_stop();
    scsp_quiet(); /* discard BIOS slot/DSP state before starting the game's driver */
    sound_ram_zero(0, 0x80000u);   /* the whole 4 Mbit: power-on contents are not zero on every console */
    driver_ok = driver_start(false);
    printf("snd: adp68k (8 ADPCM channels on the SCSP DSP), %lu KB bank, %d CD-DA tracks%s\n",
           (unsigned long)((BANK_END - BANK_OFF) / 1024), ntracks, driver_ok ? "" : ", NO DRIVER");
    return driver_ok && aud_prepare_scene(0, 0);
}

void aud_shutdown(void)
{
    aud_music_stop();
    for (int i = 0; i < CHANNELS; i++) if (voices[i].active) { voice_release(&voices[i]); voices[i].action = ACT_STOP; }
    send();
    unsigned kb = 0; for (int i = 0; i < nsamples; i++) if (samples[i].addr) kb += samples[i].bytes;
    printf("snd: %u KB of samples resident, %u late sends\n", kb / 1024, kicks_late);
}
