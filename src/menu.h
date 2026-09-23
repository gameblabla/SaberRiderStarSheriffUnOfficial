#pragma once
/* Front-end state machine (E2DM menu object DAT_00ac9be0): splashes, intro FMV, title, options, briefing, character select,
 * result screens, backer credits. State numbers are the original's (+0x58). */
#include "platform/render.h"
#include "platform/plat.h"
#include "input.h"
#include "video.h"
#include "dialog.h"

enum { MS_SPLASH0 = 0, MS_SPLASH1, MS_SPLASH2, MS_SPLASH3, MS_INTRO = 4, MS_MAIN = 5, MS_OPTIONS = 6, MS_BRIEFING = 7,
       MS_CHARSEL = 8, MS_GAMEOVER = 9, MS_LEVEL = 10, MS_ACCOMPLISHED = 0xf, MS_CREDITS = 0x10,
       MS_CONTINUE = 0x11 };   /* ours: the arcade CONTINUE? countdown before GAME OVER (the demo stored the option but never used it) */
#define CONTINUE_FROM 20     /* the countdown starts here, one per second */

enum { OPT_EXIT, OPT_LEVEL, OPT_PLAYER, OPT_CONTINUE, OPT_SCREEN, OPT_RATIO, OPT_FILTER, OPT_MUSIC, OPT_CREDITS, OPT_COUNT };
enum { RATIO_WIDE = 0, RATIO_43 = -1, RATIO_STRETCH = 1 };
enum { FILTER_NONE, FILTER_CRT, FILTER_DOUBLE, FILTER_DOUBLE_SCAN, FILTER_CRT_SCAN, FILTER_COUNT };

typedef struct {
    int state; float t, dur;   /* +0x58 state, +0x64 time, +0x60 period */
    int sel;                   /* main menu / options cursor (+0x70) */
    int character;             /* 0 Saber, 1 Fireball, 2 April, 3 Colt (+0x5c) */
    Video *video;
    Dialog dlg;                /* briefing text */
    bool start_level;          /* set when character select finished -> game starts the level */
    bool continue_now;         /* CONTINUE? accepted: the game restarts the stage with fresh lives */
    int continues_left;        /* shown on the CONTINUE? screen (the game keeps the count) */
    bool next_stage;           /* MISSION ACCOMPLISHED finished: the game decides whether another stage follows */
    bool more_stages;          /* set by the game while a stage follows: the result screen hands over instead of splashing */
    bool ending;               /* the last stage is won: MISSION ACCOMPLISHED rolls the credits, then the splash */
    int cleared_stage;         /* the stage MISSION ACCOMPLISHED celebrates (1-4): picks its art (assets/victory) */
    int idle_frames;           /* title attract timer */
    float angle;               /* rotating background (DAT_00ac9a88 / DAT_00ac9a8c) */
    int credits_page; float credits_t;
    /* settings (DAT_00aab780 difficulty, DAT_00aab788 lives, DAT_00aab784 continues, DAT_00ace3c0 screen, DAT_00aab770 ratio,
       DAT_00ace3bc filter, DAT_00ac9a9c music test) */
    int difficulty, lives, continues, screen, ratio, filter, music_track;
    bool apply_screen_mode;    /* set when screen / ratio changed */
} Menu;

void menu_enter(Menu *m, int state);
void menu_update(Menu *m, const Input *in, float dt, int sw, Ren *r);
void menu_draw(Menu *m, Ren *r, int sw, int sh);
static inline bool menu_scanlines(const Menu *m) { return m->filter == FILTER_CRT || m->filter == FILTER_DOUBLE_SCAN || m->filter == FILTER_CRT_SCAN; }
