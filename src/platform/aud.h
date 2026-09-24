#pragma once
/* Platform sound backend under the core's audio.c (which keeps all game logic: the sfx table and its random
 * variants, retrigger windows, delays, hero overrides, the voice slot, the loop's fades and the music fade/duck).
 *   platform/sdl3/aud_sdl.c        SDL3 audio streams + libvorbisfile, a limiter on the mix
 *   platform/dreamcast/aud_dc.c    AICA: snd_sfx samples (ADPCM), long ones through snd_stream, libADX music
 * Gains are linear 0..1 (the core applies its bus levels). */
#include <stdint.h>
#include <stdbool.h>
#include "../real.h"

typedef struct AudSample AudSample;

bool aud_init(void);
void aud_shutdown(void);
void aud_update(void);                           /* once per frame */

/* samples are cached by the backend; NULL if missing */
AudSample *aud_sample_pack(uint32_t id);         /* a pack sfx by resource id (RIFF, 4-bit ADPCM or PCM16) */
AudSample *aud_sample_file(const char *path);    /* one of our wav files (PCM16 in assets/; converted on consoles) */
/* have it ready from now on (no load when it first plays), however long; loop: also for playing it as a loop */
void aud_keep(AudSample *s, bool loop);
void aud_prefetch(AudSample *s);                 /* load it now, but a long one may be dropped again for others */

/* voices: a handle >= 0 or -1 when nothing could play */
int  aud_play(AudSample *s, real gain, bool loop);
void aud_set_gain(int voice, real gain);
void aud_stop(int voice);
bool aud_playing(int voice);

/* music: pack music resources (MUPS) by id; one track at a time */
bool aud_music_play(uint32_t id, bool loop);
void aud_music_stop(void);
void aud_music_gain(real g);
void aud_music_pause(bool pause);
