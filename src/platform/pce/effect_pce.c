#include "video_pce.h"
/* Frozen-scene feasibility test: a four-color prepared base remains in planes
 * 0/1, while only planes 2/3 change. Eight unique characters temporarily use
 * the font's final eight glyphs; a stage reload restores the complete font. */
__attribute__((noinline)) void effect_init(void) {
    video_scroll(0,0);video_sat_begin();video_sat_end();video_vdc(0,0);
    for(uint16_t k=0;k<2048;++k)video_vdc(2,0xf000|(PCE_FONT_WORD>>4));
    static const uint8_t base[4][3]={{0,0,0},{1,2,5},{4,2,0},{7,7,7}};
    for(uint8_t cls=0;cls<4;++cls)for(uint8_t b=0;b<4;++b) {
        uint8_t r=base[b][0]+cls,g=base[b][1]+cls,bl=base[b][2]+cls*2;
        if(r>7)r=7;if(g>7)g=7;if(bl>7)bl=7;
        pce_vce_set_color(240+cls*4+b,((uint16_t)g<<6)|(r<<3)|bl);
    }
    pce_vce_set_color(0,0);
    for(uint8_t tile=0;tile<8;++tile) {
        video_vdc(0,PCE_FONT_WORD+(88+tile)*16);
        for(uint8_t y=0;y<8;++y) {
            uint8_t p0=0,p1=0;
            for(uint8_t x=0;x<8;++x) {
                uint8_t color=((x>>2)+(y>>2)+(tile&1)+((tile>>1)&1))&3;
                p0=(p0<<1)|(color&1);p1=(p1<<1)|(color>>1);
            }
            video_vdc(2,p0|(uint16_t)p1<<8);
        }
        for(uint8_t y=0;y<8;++y)video_vdc(2,0);
        video_vdc(0,(uint16_t)(8+((tile&3)>>1))*64+(tile<4?8:20)+(tile&1));
        video_vdc(2,0xf000+(PCE_FONT_WORD>>4)+88+tile);
    }
}
__attribute__((noinline)) void effect_draw(uint8_t tick) {
    uint8_t cls=1+(tick/16)%3;
    for(uint8_t tile=0;tile<4;++tile) {
        video_vdc(0,PCE_FONT_WORD+(92+tile)*16+8);
        for(uint8_t y=0;y<8;++y) {
            uint8_t mask=0;
            for(uint8_t x=0;x<8;++x) {
                int8_t dx=x+(tile&1)*8-7,dy=y+(tile>>1)*8-7;
                mask=(mask<<1)|(dx*dx+dy*dy<49);
            }
            video_vdc(2,(cls&1?mask:0)|(uint16_t)(cls&2?mask:0)<<8);
        }
    }
    video_text(1,12,"BASE 2 PLANES / MASK 2 PLANES");
}
