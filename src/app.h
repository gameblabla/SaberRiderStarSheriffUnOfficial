#pragma once
/* The platform-independent program: opens the packs, owns the Game and runs it at a fixed 60 Hz step with the
 * debug input drivers (SABER_SCRIPT, SABER_FUZZ, SABER_FAST, SABER_SHOT). A platform's main() creates its
 * renderer, then calls app_init once and app_frame every displayed frame. */
#include "game.h"

#define APP_SCREEN_W 426
#define APP_SCREEN_H 240

bool  app_init(Ren *ren, const char *data_dir, int start_level);
/* advance by `elapsed` seconds of real time (runs 0..n fixed steps) */
void  app_update(double elapsed);
/* draw the frame (the backend presents it) */
void  app_draw(void);
/* the SABER_SHOT path when the frame just drawn is to be saved (then quit), else NULL */
const char *app_shot_path(void);
void  app_shutdown(void);
Game *app_game(void);
