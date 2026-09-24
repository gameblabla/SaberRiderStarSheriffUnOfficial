#pragma once
/* internal to the SDL3 platform */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../input.h"
void input_sdl_init(void);   /* loads the key / pad bindings */
void input_sdl_event(Input *in, const SDL_Event *ev);
/* the video's own soundtrack (video_ffmpeg.c -> aud_sdl.c) */
bool snd_sdl_music_blob(const uint8_t *mups, uint32_t size, bool loop);   /* false = not started */
void snd_sdl_sfx_blob(const uint8_t *riff, uint32_t size);
void snd_sdl_stop_blob(void);
