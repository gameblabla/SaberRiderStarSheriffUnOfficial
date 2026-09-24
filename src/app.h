#pragma once
/* The platform-independent program: opens the packs, owns the Game and runs it at a fixed 60 Hz step with the
 * debug input drivers (SABER_SCRIPT, SABER_FUZZ, SABER_FAST, SABER_SHOT). A platform's main() creates its
 * renderer, then calls app_init once and app_frame every displayed frame. */
#include "game.h"

#define APP_SCREEN_W 426
#define APP_SCREEN_H 240

bool  app_init(Ren *ren, const char *data_dir, int start_level);
/* advance by `elapsed` seconds of real time; returns the fixed steps it ran (0..n) */
int   app_update(double elapsed);
/* draw the frame (the backend presents it) */
void  app_draw(void);
/* the SABER_SHOT path when the frame just drawn is to be saved (then quit), else NULL */
const char *app_shot_path(void);
/* debug SABER_PERF: the backend reports each frame's update / draw / whole-frame time (microseconds), the steps it
 * ran and its draw count (-1 unknown); a line a second goes to stderr (the serial log on the Dreamcast) */
bool  app_perf_on(void);
void  app_perf(uint32_t upd_us, uint32_t draw_us, uint32_t frame_us, int steps, int prims);
void  app_shutdown(void);
Game *app_game(void);
