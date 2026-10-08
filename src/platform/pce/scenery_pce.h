#pragma once
#include "pce_config.h"
#include "sgx_pce.h"
#define PCE_SCENERY __attribute__((noinline,minsize,section(".ram_bank120.text")))
extern uint8_t space_flashing,space_hull_ready,space_hull_top[28],space_hull_bottom[28];
void space_hull_load(void),space_hull_bat(void);
void space_beam(uint8_t width);
extern int16_t space_hull_x;   /* the hull's offset to the right of its place (the cruiser flying in) */
void space_hull_draw(int16_t y,uint8_t flash,bool gone);
void space_screen_flash(uint8_t frames);
void space_screen_flash_end(void);
/* Match the visible hull rather than the former 128x64 rectangle. */
static inline __attribute__((always_inline)) bool space_hull_hit(int16_t x,int16_t y) {
    uint8_t left=pce_sgx_gameplay()?16:76,width=pce_sgx_gameplay()?224:180;
    uint8_t height=pce_sgx_gameplay()?123:98;
    if(!space_hull_ready||x<left||x>=left+width||y<0||y>=height)return false;
    uint8_t c=(x-left)>>3;
    return space_hull_top[c]!=255&&y>=space_hull_top[c]&&y<=space_hull_bottom[c];
}

static inline __attribute__((always_inline)) int16_t space_hull_cannon_x(void) {
    return pce_sgx_gameplay()?26:84;
}
static inline __attribute__((always_inline)) int16_t space_hull_cannon_y(void) {
    return pce_sgx_gameplay()?56:48;
}
