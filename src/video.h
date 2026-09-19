#pragma once
/* E2DM .vid playback (XviD elementary stream + RIFF ADPCM / MUPS audio) via libavcodec. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct Video Video;
Video *video_open(SDL_Renderer *r, uint32_t id);
bool   video_update(Video *v, float dt);          /* returns false when finished */
void   video_draw(Video *v, SDL_Renderer *r, int sw, int sh);   /* letterboxed to the screen */
void   video_close(Video *v);
