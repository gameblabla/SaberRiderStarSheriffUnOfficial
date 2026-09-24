#pragma once
/* Game audio (FUN_00425a20 / FUN_00425ce0 semantics): the sfx table with its random variants and retrigger windows,
 * the music table, our own wav voice lines. Playback goes through the platform backend (platform/snd.h). */
#include <stdint.h>
#include <stdbool.h>
bool audio_init(void);
void audio_shutdown(void);
void sfx_play(int game_id, int delay_frames);   /* FUN_00425a20 semantics (random variants) */
void sfx_play_id(uint32_t resource_id);        /* play an sfx by pack id (dialog scripts) */
void music_play(int index, bool loop);          /* FUN_00425ce0 table index */
void music_stop(void);
void music_set_volume(float v);                 /* FUN_00425e70: 0..1 fade factor on top of the track's own volume */
void music_set_duck(float v);                   /* temporary power-attack music level; ramps without stopping playback */
void music_pause(bool pause);                   /* FUN_00411300 / FUN_004113b0 (pause screen) */
void voice_play_file(const char *path);         /* our own PCM16 wav voice line (stops the previous one) */
void voice_stop(void);
void sfx_play_file(const char *path);           /* one-shot of our own PCM16 wav (turbo ignition) */
void sfx_loop(const char *path);
void sfx_preload_file(const char *path);        /* load one of our samples now (a stage's own sounds at its start) */
void sfx_preload_loop(const char *path);        /* the same for one sfx_loop will play */
void sfx_prefetch_id(uint32_t resource_id);     /* load a pack sample now, long ones into the cache they may leave again (a dialog's voice) */                /* keep a wav looping (turbo roar); NULL fades it out */
void sfx_set_override(int game_id, const char *const *paths, int n);   /* hero grunts: replace a game sfx id by our wav files (random pick) */
void sfx_clear_overrides(void);
void audio_update(void);                        /* per frame: delayed sfx, retrigger windows, loop fades, music duck */
