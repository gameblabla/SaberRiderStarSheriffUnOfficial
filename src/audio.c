#include "audio.h"
#include "platform/aud.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- mix levels ----
 * The original (FUN_00563900 / FUN_00563780) sums the music at vol/256 and every sfx voice at unity into 16-bit
 * and hard-clips at +-0x7fbc. The material is mastered hot (most sfx peak at 0 dBFS, the music tracks at
 * 0..+1 dB), so a voice line over a gunshot over the level track clips audibly. Instead of reproducing that,
 * each bus gets some headroom (and the PC backend runs a peak limiter on the mix). */
#define GAIN_SFX   0.60f   /* game sfx: -4.4 dB */
#define GAIN_VOICE 0.80f   /* dialog / video speech: -1.9 dB */
#define GAIN_MUSIC 0.90f   /* on top of the music table's per-track volume and the fade */

/* ---- sfx ---- */
static const uint32_t SFX_TABLE[32] = {   /* 0x7c5480 */
    0xE418A101, 0xEB3309DC, 0xEB450AED, 0x8ADE82B6, 0x8ACB81A0, 0x8AB88092, 0x9C7B3FD9, 0xC66E1894,
    0xC6801BB9, 0xBF4917FF, 0xBF5B14EA, 0xBF6D1599, 0x89389611, 0x8923950D, 0xFE10EB78, 0xFDFFE848,
    0xA8382083, 0x162864B4, 0x8BE8F136, 0xF8B5C0E8, 0x208C64D9, 0xE73A3850, 0x87265BA0, 0x1DBF470E,
    0x8AEB8147, 0xF11FCC31, 0x0AFC505A, 0x15A00BA1, 0x82EFBA26, 0x47D886A1, 0xE105C92A, 0xABC6A6E8 };
static const uint32_t MUSIC_TABLE[18] = {   /* 0x7c53a0 */
    0xD9F22466, 0x70090142, 0x39E19337, 0x086C02EE, 0x3E0E8A02, 0xEABD3205, 0xAF162C58, 0xC93B9F7F, 0xB10AB207,
    0x25930DD9, 0xD50E6105, 0xD518620C, 0xD52263F3, 0xD52C64FE, 0xD53665E9, 0xD54066D0, 0xD54A67DF, 0xD55468CA };
static const float MUSIC_VOL[18] = { 1, 1, 1, 1, 1, 0.85f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };   /* 0x7c53a8 */

typedef struct { int table_idx; int frame; } Delayed;
static Delayed delayed[64]; static int ndelayed;
static uint32_t frame_counter;

static void play_table_now(int idx) { aud_play(aud_sample_pack(SFX_TABLE[idx]), GAIN_SFX, false); }

void sfx_play_id(uint32_t id) { aud_play(aud_sample_pack(id), GAIN_VOICE, false); }

/* FUN_00425880 / FUN_004109a0: every table sample owns one slot. A request opens (or extends) a window of
 * len*3 ms; inside a window the sample plays at most once per len*6 ms, so hammering the trigger (auto-fire,
 * a burst of hits) does not stack identical copies or eat all the voices, which is what starved other sounds. */
static const int SFX_LEN[32] = {   /* 0x7c5484: retrigger length units */
    40, 60, 60, 60, 60, 60, 40, 36, 36, 16, 16, 16, 20, 20, 12, 12,
    40, 40, 40, 40, 40, 20, 40, 40, 36, 18, 36, 60, 200, 60, 60, 50 };
static struct { uint32_t end, next; } slot[32];

static void service_slots(void)
{
    uint32_t now = (uint32_t)plat_ticks_ms();
    for (int i = 0; i < 32; i++) {
        if (!slot[i].end) continue;
        if ((int32_t)(now - slot[i].end) >= 0) { slot[i].end = 0; continue; }
        if ((int32_t)(now - slot[i].next) >= 0) { play_table_now(i); slot[i].next = now + SFX_LEN[i] * 6; }
    }
}

static void play_table(int idx)
{
    if (idx < 0 || idx >= 32) return;
    uint32_t now = (uint32_t)plat_ticks_ms();
    if (!slot[idx].end) slot[idx].next = now;
    slot[idx].end = now + SFX_LEN[idx] * 3 - 3;
    service_slots();
}

static int rnd(int n) { return rand() % (n + 1); }   /* FUN_0040cf30(0, n) inclusive per the switch usage */

static struct { int n; char file[4][256]; } sfx_over[24];   /* per-hero replacements for the player's grunts (game sfx ids) */

void sfx_play(int id, int delay)
{
    if (id >= 0 && id < 24 && sfx_over[id].n) {
        const char *f = sfx_over[id].file[rnd(sfx_over[id].n - 1)];
        if (plat_getenv("SABER_TRACE")) fprintf(stderr, "sfx %d -> %s\n", id, f);
        aud_play(aud_sample_file(f), GAIN_SFX, false);
        return;
    }
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

/* picked at random like the original's variants */
void sfx_set_override(int game_id, const char *const *paths, int n)
{
    if (game_id < 0 || game_id >= 24) return;
    sfx_over[game_id].n = 0;
    for (int i = 0; i < n && i < 4; i++) if (paths[i]) snprintf(sfx_over[game_id].file[sfx_over[game_id].n++], 256, "%s", paths[i]);
}
void sfx_clear_overrides(void) { memset(sfx_over, 0, sizeof sfx_over); }

/* ---- our own samples ---- */
static int file_voice = -1;
void voice_stop(void) { if (file_voice >= 0) { aud_stop(file_voice); file_voice = -1; } }
void voice_play_file(const char *path)
{
    voice_stop();
    if (!path) return;
    if (plat_getenv("SABER_TRACE")) fprintf(stderr, "voice %s\n", path);
    file_voice = aud_play(aud_sample_file(path), GAIN_VOICE, false);
}

void sfx_play_file(const char *path) { if (path) aud_play(aud_sample_file(path), GAIN_SFX, false); }

/* a looping sample: audio_update fades it in while it is wanted and out (~0.15 s) when it is not, then stops it
 * so a re-start begins from the loop's head */
static int loop_voice = -1; static AudSample *loop_smp; static float loop_gain; static bool loop_want;
void sfx_loop(const char *path)
{
    if (!path) { loop_want = false; return; }
    AudSample *s = aud_sample_file(path); if (!s) return;
    if (loop_voice >= 0 && loop_smp != s) { aud_stop(loop_voice); loop_voice = -1; }
    if (loop_voice < 0) { loop_gain = 0; loop_voice = aud_play(s, 0.0f, true); if (loop_voice < 0) return; }
    loop_smp = s; loop_want = true;
}
static void service_loop(void)
{
    if (loop_voice < 0) return;
    float target = loop_want ? 1.0f : 0.0f;
    loop_gain += (target - loop_gain) * (loop_want ? 0.35f : 0.2f);
    if (!loop_want && loop_gain < 0.02f) { loop_gain = 0; aud_stop(loop_voice); loop_voice = -1; return; }
    aud_set_gain(loop_voice, loop_gain * GAIN_SFX);
}

/* ---- music ---- */
static float music_track_vol = 1.0f, music_fade = 1.0f;   /* 0x7c53a8 per-track volume x FUN_00425e70 fade */
static float music_duck = 1.0f, music_duck_target = 1.0f;

static void apply_music_gain(void)
{
    /* the mixer's 0..255 stream volume is not linear: a 1 s fade in the original is inaudible after ~0.7 s,
     * which a squared curve reproduces (measured on a pulse capture of the original's character select) */
    float v = music_fade < 0.001f ? 0 : music_fade > 0.999f ? 1.0f : music_fade;
    aud_music_gain(v * v * music_track_vol * GAIN_MUSIC * music_duck);
}

void music_set_volume(float v) { music_fade = v < 0 ? 0 : v > 1 ? 1 : v; apply_music_gain(); }
void music_set_duck(float v) { music_duck_target = v < 0 ? 0 : v > 1 ? 1 : v; }
void music_pause(bool pause) { aud_music_pause(pause); }
void music_stop(void) { aud_music_stop(); }

void music_play(int index, bool loop)
{
    aud_music_stop();
    if (index < 0 || index >= 18) return;
    music_track_vol = MUSIC_VOL[index]; music_fade = 1.0f; music_duck = music_duck_target = 1.0f;
    apply_music_gain();
    if (aud_music_play(MUSIC_TABLE[index], loop)) apply_music_gain();
}

void audio_update(void)
{
    /* A fast dip makes the sting and line intelligible; the longer return avoids a volume jump at the hit.
     * This only changes gain: the music keeps its place through the whole cut-in. */
    float step = music_duck_target < music_duck ? 0.28f : 0.075f;
    music_duck += (music_duck_target - music_duck) * step;
    if (fabsf(music_duck_target - music_duck) < 0.001f) music_duck = music_duck_target;
    apply_music_gain();
    frame_counter++;
    for (int i = 0; i < ndelayed; ) {
        if ((int)(frame_counter - delayed[i].frame) >= 0) { play_table(delayed[i].table_idx); delayed[i] = delayed[--ndelayed]; }
        else i++;
    }
    service_slots(); service_loop();
    aud_update();
}

bool audio_init(void) { return aud_init(); }
void audio_shutdown(void) { aud_music_stop(); aud_shutdown(); }
