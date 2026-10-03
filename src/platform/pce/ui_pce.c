#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "audio_pcm.h"
#define UI_BASE __attribute__((noinline,section(".ram_bank106.text")))
#define UI_SPRITE_CODE (UI_SPRITE_WORD>>5)
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_count;
extern uint8_t previous;
extern volatile uint16_t pce_scroll_x;
volatile uint8_t pce_ui_state;
const PceUiScreen *ui_screen;
uint16_t ui_ramp[4][12];
uint16_t ui_ring[4][16];
uint8_t ui_held,ui_pressed;
uint8_t ui_cycle_step,ui_cycle_clock;

UI_BASE void ui_read_keys(void) {
    ui_held=~pce_joypad_read();ui_pressed=ui_held&~previous;previous=ui_held;
}
UI_BASE void ui_blip(void) { audio_pcm_play(1); }
UI_BASE void ui_sprite(int16_t x,int16_t y,uint16_t pattern,uint8_t palette,bool wide) {
    if(sat_count>=64||x<=-32||x>=320||y<=-16||y>=224)return;
    sat[0][sat_count++]=(vdc_sprite_t){y+64,x+32,UI_SPRITE_CODE+pattern*2,
        VDC_SPRITE_FG|palette|(wide?VDC_SPRITE_WIDTH_32:0)};
}
UI_BASE void ui_vram(uint32_t address,uint16_t word,uint32_t bytes) {
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    while(bytes) {
        uint16_t n=bytes>32768?32768:bytes;
        arcade_vram(address,word,n);address+=n;word+=n>>1;bytes-=n;
    }
}
/* Build and load a screen: palettes, characters, BAT, sprite patterns. */
UI_BASE void ui_show(uint8_t id) {
    const PceUiScreen *s=ui_screen=&pce_ui[id];
    video_display(false);pce_raster_enabled=0;pce_scroll_x=0;
    video_mode_ui();
    arcade_read(2,s->pal,buffer,512);pce_vce_copy_palette(0,buffer,16);
    arcade_read(2,s->sprpal,buffer,512);pce_vce_copy_palette(16,buffer,16);
    ui_vram(s->tiles,UI_TILE_WORD,(uint32_t)s->ntiles*32);
    for(uint8_t row=0;row<28;++row)ui_vram(s->map+(uint32_t)row*80,(uint16_t)row*64,80);
    if(s->nsprpat)ui_vram(s->sprpat,UI_SPRITE_WORD,(uint32_t)s->nsprpat*128);
    if(id!=SCREEN_TITLE) {
        arcade_read(2,s->extra,ui_ramp,PCE_UI_RAMP_BYTES);
        arcade_read(2,s->pal,ui_ring,128);
    }
    ui_cycle_step=ui_cycle_clock=0;
    video_sat_begin();video_sat_end();
    video_display(true);
}
UI_BASE void ui_put(uint8_t col,uint8_t row,const char *text,uint8_t slot) {
    pce_cpu_irq_disable();
    *(volatile uint8_t*)0x20f7=0;*IO_VDC_INDEX=0;
    uint16_t address=(uint16_t)row*64+col;
    *IO_VDC_DATA_LO=address;*IO_VDC_DATA_HI=address>>8;
    *(volatile uint8_t*)0x20f7=2;*IO_VDC_INDEX=2;
    while(*text) {
        uint8_t c=*text++;
        uint16_t tile=(c<=32||c>126)?0:c-32;
        uint16_t word=((uint16_t)slot<<12)|((UI_TILE_WORD>>4)+tile);
        *IO_VDC_DATA_LO=word;*IO_VDC_DATA_HI=word>>8;
    }
    pce_cpu_irq_enable();
}
UI_BASE void ui_end(void) { video_sat_begin();video_sat_end(); }
