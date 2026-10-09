#pragma once
#include "pce_config.h"
#include "assets.h"
#include "overlay_pce.h"
/* Shared front-end services. They live in the renderer bank ($6a), which is
 * always mapped, so every front-end overlay can call them directly. */
#define UI_TILE_WORD 0x0800
#define UI_SPRITE_WORD 0x6800
enum { SCREEN_TITLE, SCREEN_SELECT, SCREEN_OPTIONS, SCREEN_PANEL, SCREEN_GAMEOVER };
extern volatile uint8_t pce_ui_state;
extern const PceUiScreen *ui_screen;
extern uint8_t ui_held,ui_pressed;
void ui_read_keys(void);
void ui_blip(void);
void ui_sprite(int16_t x,int16_t y,uint16_t pattern,uint8_t palette,bool wide);
void ui_vram(void);
extern uint8_t buffer[2048];
static inline void ui_vram_load(uint32_t address,uint16_t word,uint32_t bytes) {
    uint32_t *args=(uint32_t *)buffer;
    args[0]=address;((uint16_t *)buffer)[2]=word;args[2]=bytes;
    overlay_call(0x71,ui_vram);
}
void ui_show(uint8_t id);
void ui_put(uint8_t col,uint8_t row,const char *text,uint8_t slot);
void ui_end(void);
/* Palette fade through black. ui_fade(8) snapshots every palette; ui_fade(n) shows the snapshot darkened by n steps
 * (0 = as shown, 7 = black). With ui_dark set, ui_show leaves the display off so a fade-in starts from black. */
extern uint8_t ui_dark;
void ui_fade(uint8_t level);
void ui_fade_out_body(void),ui_black_body(void),ui_fade_in_body(void);
static inline __attribute__((always_inline)) void ui_fade_out(void) {overlay_call(0x7b,ui_fade_out_body);}   /* the screen fades to black (a common call: front end and every stage) */
static inline __attribute__((always_inline)) void ui_black(void) {overlay_call(0x7b,ui_black_body);}         /* snapshot the palettes of a screen that has been set up, and go black under it */
static inline __attribute__((always_inline)) void ui_fade_in(void) {overlay_call(0x7b,ui_fade_in_body);}     /* then turn the display on and bring the screen up from black */
extern uint16_t ui_ramp[4][12],ui_ring[4][16];
extern uint8_t ui_cycle_step,ui_cycle_clock;
/* Small helpers compiled into each overlay to spare the renderer bank. */
static inline __attribute__((always_inline)) void ui_put_number(uint8_t col,uint8_t row,uint8_t n,uint8_t slot) {
    /* Keep address-taken text out of compiler zero-page temporaries: their
     * offsets are not valid generic pointers on the HuC6280 ($2000 base). */
    static char text[3];
    text[0]='0'+n/10%10;text[1]='0'+n%10;text[2]=0;
    ui_put(col,row,text,slot);
}
static inline void ui_clear_rows(uint8_t first,uint8_t last) {
    for(uint8_t r=first;r<=last;++r)ui_put(4,r,"                                ",12);
}
/* Rotate the tunnel colours: one step every few frames. */
void ui_cycle(void);
