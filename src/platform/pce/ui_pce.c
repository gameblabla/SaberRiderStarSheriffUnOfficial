#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "audio_pcm.h"
#include "sgx_pce.h"
#define UI_BASE __attribute__((noinline,section(".ram_bank106.text")))
#define UI_VRAM_CODE __attribute__((noinline,minsize,section(".ram_bank111.text")))
#define UI_CYCLE_CODE __attribute__((noinline,minsize,section(".ram_bank111.text")))
#define UI_SPRITE_CODE (UI_SPRITE_WORD>>5)
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_count;
extern uint8_t previous;
extern volatile uint16_t pce_scroll_x,pce_scroll_y;
volatile uint8_t pce_ui_state;
const PceUiScreen *ui_screen;
uint16_t ui_ramp[4][12];
uint16_t ui_ring[4][16];
uint8_t ui_held,ui_pressed;
uint8_t ui_cycle_step,ui_cycle_clock,ui_dark;

UI_CYCLE_CODE static void ui_cycle_body(void) {
    if(++ui_cycle_clock<4)return;
    ui_cycle_clock=0;if(++ui_cycle_step==12)ui_cycle_step=0;
    for(uint8_t k=0;k<4;++k)for(uint8_t i=0;i<12;++i) {
        uint8_t j=i+ui_cycle_step;if(j>=12)j-=12;
        ui_ring[k][1+i]=ui_ramp[k][j];
    }
    pce_vce_copy_palette(0,ui_ring,4);
}
void ui_cycle(void) { overlay_call(0x6f,ui_cycle_body); }

UI_BASE void ui_read_keys(void) {
    ui_held=~pce_joypad_read();ui_pressed=ui_held&~previous;previous=ui_held;
}
UI_BASE void ui_blip(void) { audio_pcm_tick(); }   /* the PC menus' tick (sfx 0) */
UI_BASE void ui_sprite(int16_t x,int16_t y,uint16_t pattern,uint8_t palette,bool wide) {
    if(sat_count>=64||x<=-32||x>=320||y<=-16||y>=224)return;
    sat[0][sat_count++]=(vdc_sprite_t){y+64,x+32,UI_SPRITE_CODE+pattern*2,
        VDC_SPRITE_FG|palette|(wide?VDC_SPRITE_WIDTH_32:0)};
}
UI_VRAM_CODE void ui_vram(void) {
    uint32_t *args=(uint32_t *)buffer;
    uint32_t address=args[0],bytes=args[2];
    uint16_t word=((uint16_t *)buffer)[2];
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    while(bytes) {
        uint16_t n=bytes>32768?32768:bytes;
        arcade_vram(address,word,n);address+=n;word+=n>>1;bytes-=n;
    }
}
/* Build and load a screen: palettes, characters, BAT, sprite patterns. */
UI_BASE void ui_show(uint8_t id) {
    const PceUiScreen *s=ui_screen=&pce_ui[id];
    video_display(false);pce_raster_enabled=0;pce_scroll_x=pce_scroll_y=0;
    video_mode_ui();
#ifdef PCE_SGX
    pce_sgx_metrics.paired_screen=id;
    overlay_call(0x7c,pce_sgx_ui_load_body);
#else
    arcade_read(2,s->pal,buffer,512);pce_vce_copy_palette(0,buffer,16);
    arcade_read(2,s->sprpal,buffer,512);pce_vce_copy_palette(16,buffer,16);
    ui_vram_load(s->tiles,UI_TILE_WORD,(uint32_t)s->ntiles*32);
    for(uint8_t row=0;row<28;++row)ui_vram_load(s->map+(uint32_t)row*80,(uint16_t)row*64,80);
    if(s->nsprpat)ui_vram_load(s->sprpat,UI_SPRITE_WORD,(uint32_t)s->nsprpat*128);
#endif
    if(id!=SCREEN_TITLE) {
        arcade_read(2,s->extra,ui_ramp,PCE_UI_RAMP_BYTES);
        arcade_read(2,s->pal,ui_ring,128);
    }
    ui_cycle_step=ui_cycle_clock=0;
    video_sat_begin();video_sat_end();
    if(!ui_dark)video_display(true);
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
UI_BASE void ui_end(void) {
    video_sat_begin();video_sat_end();
#ifdef PCE_SGX
    overlay_call(0x7c,pce_sgx_ui_end_body);
#endif
}
UI_BASE void ui_fade(uint8_t level) {
    uint16_t *src=(uint16_t*)buffer,*dst=src+512;
    if(level==8){pce_vce_copy_palette_to_ram(buffer,0,32);return;}
    for(uint16_t i=0;i<512;++i) {
        uint16_t c=src[i];uint8_t b=c&7,r=(c>>3)&7,g=(c>>6)&7;
        b=b>level?b-level:0;r=r>level?r-level:0;g=g>level?g-level:0;
        dst[i]=(uint16_t)g<<6|r<<3|b;
    }
    pce_vce_copy_palette(0,dst,32);
}
/* The common screen transitions (title -> hero select, hero select -> the stage card, every stage's start): fade out takes what is on
 * screen to black through the palette snapshot; a screen is shown from black by ui_black (the snapshot of the palettes it was built
 * with, then black) once it is set up, and ui_fade_in brings it up. The snapshot lives in `buffer`: nothing else may use it between.
 * They are in the spare bank $7b (ui_pce.h has the calls) because the renderer bank and the front end's are full. */
#define UI_FADE_CODE __attribute__((noinline,section(".ram_bank123.text")))
UI_FADE_CODE static void fade_step(uint8_t level) {video_wait();video_wait();video_wait();ui_fade(level);}
UI_FADE_CODE void ui_fade_out_body(void) {ui_fade(8);for(uint8_t level=1;level<8;++level)fade_step(level);}
UI_FADE_CODE void ui_black_body(void) {ui_fade(8);ui_fade(7);}
UI_FADE_CODE void ui_fade_in_body(void) {
    video_display(true);
#ifdef PCE_SGX
    overlay_call(0x72,pce_sgx_display_on_body);
#endif
    for(uint8_t level=6;level<7;--level)fade_step(level);
}
