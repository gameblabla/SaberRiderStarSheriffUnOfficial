#include "presentation_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#define PRESENT __attribute__((noinline,section(".ram_bank116.text")))
PRESENT void presentation_frame(void) {
    const uint16_t *base=pce_present_base[pce_metrics.stage-1];
    if(base[0]) {
        uint8_t hp=pce_metrics.hp>3?3:pce_metrics.hp;
        video_sprite(base[1]+pce_campaign.lives%10,29,14,false,16);
        video_sprite(base[1]+pce_campaign.powers%10,60,14,false,16);
        video_sprite(base[0]+pce_control.hero*4+hp,0,0,false,16);
    }
}
void presentation_draw(void) {overlay_call(0x74,presentation_frame);video_front_mark();}

static uint8_t panel_x,panel_y,panel_w,panel_h;
PRESENT static void panel_draw(void) {
    uint8_t x=panel_x,y=panel_y,w=panel_w,h=panel_h;
    extern volatile uint16_t pce_scroll_x;
    for(uint8_t row=0;row<h;++row)for(uint8_t col=0;col<w;++col) {
        uint16_t address=pce_raster_enabled?(uint16_t)(48+y+row)*128+x+col:
            (uint16_t)(y+row)*64+(((pce_scroll_x>>3)+x+col)&63);
        video_vdc(0,address);video_vdc(2,0xf000+(PCE_FONT_WORD>>4));
    }
}

/* Blank BG cells (the font's space glyph) where a sprite panel sits behind the text layer. */
void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h) {
    panel_x=x;panel_y=y;panel_w=w;panel_h=h;overlay_call(0x74,panel_draw);
}
