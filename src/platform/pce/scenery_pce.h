#pragma once
#include "pce_config.h"
#define PCE_SCENERY __attribute__((noinline,section(".ram_bank120.text")))
extern uint8_t space_flashing,space_hull_ready,space_hull_top[24],space_hull_bottom[24];
void space_hull_load(void);
void space_hull_draw(int16_t y,uint8_t flash,bool gone);
void space_screen_flash(uint8_t frames);
/* Match the visible hull rather than the former 128x64 rectangle. */
static inline bool space_hull_hit(int16_t x,int16_t y) {
    if(!space_hull_ready||x<76||x>=256||y<0||y>=98)return false;
    uint8_t c=(x-76)>>3;
    return space_hull_top[c]!=255&&y>=space_hull_top[c]&&y<=space_hull_bottom[c];
}

extern uint8_t dialog_corner_colour,dialog_corner_y;
void dialog_corners(void);
void dialog_palette_prepare(void);
