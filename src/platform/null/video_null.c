/* video.h with no videos (headless tests, a port's bring-up): every open fails, as a missing clip does */
#include "../../video.h"
#include <stddef.h>

Video *video_open(Ren *r, uint32_t id) { (void)r; (void)id; return NULL; }
Video *video_open_file(Ren *r, const char *path, real fps) { (void)r; (void)path; (void)fps; return NULL; }
bool   video_update(Video *v, real dt) { (void)v; (void)dt; return false; }
void   video_draw(Video *v, Ren *r, int sw, int sh) { (void)v; (void)r; (void)sw; (void)sh; }
void   video_draw_rect(Video *v, Ren *r, real x, real y, real w, real h) { (void)v; (void)r; (void)x; (void)y; (void)w; (void)h; }
void   video_size(const Video *v, int *w, int *h) { (void)v; if (w) *w = 0; if (h) *h = 0; }
void   video_close(Video *v) { (void)v; }
