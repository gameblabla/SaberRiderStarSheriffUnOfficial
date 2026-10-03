#pragma once
#include "pce_config.h"
#include "assets.h"
#include "overlay_pce.h"
/* Shared front-end services. They live in the renderer bank ($6a), which is
 * always mapped, so every front-end overlay can call them directly. */
#define UI_TILE_WORD 0x0800
#define UI_SPRITE_WORD 0x6800
enum { SCREEN_TITLE, SCREEN_SELECT, SCREEN_OPTIONS };
extern volatile uint8_t pce_ui_state;
extern const PceUiScreen *ui_screen;
extern uint8_t ui_held,ui_pressed;
void ui_read_keys(void);
void ui_blip(void);
void ui_sprite(int16_t x,int16_t y,uint16_t pattern,uint8_t palette,bool wide);
void ui_vram(uint32_t address,uint16_t word,uint32_t bytes);
void ui_show(uint8_t id);
void ui_put(uint8_t col,uint8_t row,const char *text,uint8_t slot);
void ui_end(void);
extern uint16_t ui_ramp[4][12],ui_ring[4][16];
extern uint8_t ui_cycle_step,ui_cycle_clock;
/* Small helpers compiled into each overlay to spare the renderer bank. */
static inline void ui_put_number(uint8_t col,uint8_t row,uint8_t n,uint8_t slot) {
    char text[3]={'0'+n/10%10,'0'+n%10,0};ui_put(col,row,text,slot);
}
static inline void ui_clear_rows(uint8_t first,uint8_t last) {
    static const char blank[33]="                                ";
    for(uint8_t r=first;r<=last;++r)ui_put(4,r,blank,12);
}
/* Rotate the tunnel colours: one step every few frames. */
static inline void ui_cycle(void) {
    if(++ui_cycle_clock<4)return;
    ui_cycle_clock=0;if(++ui_cycle_step==12)ui_cycle_step=0;
    for(uint8_t k=0;k<4;++k)for(uint8_t i=0;i<12;++i) {
        uint8_t j=i+ui_cycle_step;if(j>=12)j-=12;
        ui_ring[k][1+i]=ui_ramp[k][j];
    }
    pce_vce_copy_palette(0,ui_ring,4);
}
