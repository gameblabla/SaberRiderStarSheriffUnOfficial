/* video.h for the WASM port: no videos.
 *
 * The demo's FMVs (menu.pck's intro and the briefings) are XviD in an E2DM .vid container, and the hero's power
 * cut-ins are MPEG-4 part 2 (assets/power, the .m4v files). Decoding either in the browser means shipping a codec, and
 * decoding either in the module means shipping one too - there is no emscripten here to bring libavcodec along,
 * and a from-scratch MPEG-4 part 2 decoder is not a thing to add to a game port. So this is the same stub the
 * headless build uses (platform/null/video_null.c), which the core is written to survive: the front end skips an
 * intro it cannot open (menu.c's MS_INTRO falls through to the main menu) and a power cut-in falls back to its
 * sound alone (power.c). The rest of the game - the whole demo's stages, the Mode 7 Grand Prix, Ramrod's cockpit
 * and the final phase - is unaffected, and no video.pck needs downloading.
 *
 * When a decoder is added it drops in here: the same eight functions, opening either a pack video by id
 * (video_open) or one of our own clips (video_open_file). */
#include "../../video.h"
#include <stddef.h>

Video *video_open(Ren *r, uint32_t id) { (void)r; (void)id; return NULL; }
void video_preload_file(const char *path, real fps) { (void)path; (void)fps; }
Video *video_open_file(Ren *r, const char *path, real fps) { (void)r; (void)path; (void)fps; return NULL; }
bool   video_update(Video *v, real dt) { (void)v; (void)dt; return false; }
void   video_draw(Video *v, Ren *r, int sw, int sh) { (void)v; (void)r; (void)sw; (void)sh; }
void   video_draw_rect(Video *v, Ren *r, real x, real y, real w, real h) { (void)v; (void)r; (void)x; (void)y; (void)w; (void)h; }
void   video_size(const Video *v, int *w, int *h) { (void)v; if (w) *w = 0; if (h) *h = 0; }
void   video_close(Video *v) { (void)v; }
