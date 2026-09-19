#pragma once
/* SFX (Yamaha ADPCM in RIFF, LZO1Z-packed) mixer + MUPS (renamed Ogg Vorbis) music streaming. */
#include <stdint.h>
#include <stdbool.h>
bool audio_init(void);
void audio_shutdown(void);
void sfx_play(int game_id, int delay_frames);   /* FUN_00425a20 semantics (random variants) */
void music_play(int index, bool loop);          /* FUN_00425ce0 table index */
void music_stop(void);
void audio_update(void);                        /* per frame: delayed sfx, music refill */
