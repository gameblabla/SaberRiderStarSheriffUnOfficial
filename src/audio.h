#pragma once
/* SFX (Yamaha ADPCM in RIFF, LZO1Z-packed) mixer + MUPS (renamed Ogg Vorbis) music streaming. */
#include <stdint.h>
#include <stdbool.h>
bool audio_init(void);
void audio_shutdown(void);
void sfx_play(int game_id, int delay_frames);
void sfx_play_id(uint32_t resource_id);        /* play an sfx by pack id (dialog scripts) */   /* FUN_00425a20 semantics (random variants) */
void music_play(int index, bool loop);          /* FUN_00425ce0 table index */
void music_stop(void);
void music_set_volume(float v);                 /* FUN_00425e70: 0..1 fade factor on top of the track's own volume */
void music_pause(bool pause);                   /* FUN_00411300 / FUN_004113b0 (pause screen) */
void sfx_stop_blob(void);                       /* stop a video's RIFF audio (video closed) */
bool music_play_blob(const uint8_t *mups, uint32_t size, bool loop);   /* MUPS data not in the music table (video audio); false = not started */
void sfx_play_blob(const uint8_t *riff, uint32_t size);                 /* RIFF ADPCM blob (video audio) */
void audio_update(void);                        /* per frame: delayed sfx, music refill */
