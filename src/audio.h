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
void music_play_blob(const uint8_t *mups, uint32_t size, bool loop);   /* MUPS data not in the music table (video audio) */
void sfx_play_blob(const uint8_t *riff, uint32_t size);                 /* RIFF ADPCM blob (video audio) */
void audio_update(void);                        /* per frame: delayed sfx, music refill */
