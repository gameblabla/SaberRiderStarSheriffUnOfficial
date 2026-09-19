#pragma once
/* Front-end states (splash, intro, main menu, character select, briefing, game over, mission accomplished). */
#include <SDL3/SDL.h>
#include "input.h"
#include "video.h"

enum { MS_SPLASH0 = 0, MS_SPLASH1, MS_SPLASH2, MS_SPLASH3, MS_INTRO = 4, MS_MAIN = 5, MS_OPTIONS = 6, MS_BRIEFING = 7,
       MS_CHARSEL = 8, MS_GAMEOVER = 9, MS_LEVEL = 10, MS_ACCOMPLISHED = 0xf, MS_CREDITS = 0x10 };

typedef struct {
    int state; float t, dur;
    int sel, character;
    Video *video;
    bool start_level;      /* set when the briefing finished -> game starts the level */
    float text_chars;
    int opt_sel, music_track, credits_page; bool scanlines, mode43;
    bool apply_screen_mode;   /* set when the 4:3/16:9 toggle changed */
} Menu;

void menu_enter(Menu *m, int state);
void menu_update(Menu *m, const Input *in, float dt, int sw, SDL_Renderer *r);
void menu_draw(Menu *m, SDL_Renderer *r, int sw, int sh);
