#pragma once
/* Full-motion video. Implemented per platform (platform/<name>/video_*.c): the PC decodes the demo's E2DM .vid
 * (XviD + RIFF ADPCM / MUPS audio) and our MPEG-4 clips with libavcodec; the Dreamcast plays the same videos
 * converted to DCMV (VQ textures + AICA ADPCM) by tools/dc/build_disc.py. A video plays its own soundtrack. */
#include "real.h"
#include "platform/render.h"
#include <stdint.h>
#include <stdbool.h>
typedef struct Video Video;
Video *video_open(Ren *r, uint32_t id);          /* a pack video by resource id */
/* one of our own clips (assets/, e.g. "power/fireball.m4v"): a raw MPEG-4 part 2 stream (ffmpeg -f m4v), 320x240,
 * no sound (play it alongside) */
Video *video_open_file(Ren *r, const char *path, real fps);
void   video_preload_file(const char *path, real fps); /* optional platform warm-up; NULL discards */
bool   video_update(Video *v, real dt);          /* returns false when finished */
void   video_draw(Video *v, Ren *r, int sw, int sh);   /* letterboxed to the screen */
void   video_draw_rect(Video *v, Ren *r, real x, real y, real w, real h);
void   video_size(const Video *v, int *w, int *h);
void   video_close(Video *v);
