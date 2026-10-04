#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "assets.h"
/* Scaled and cockpit-clipped sprite pieces. This slow path lives in the $71
 * overlay so the renderer bank keeps room for its hot paths. */
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page, sat_count, descriptor[384], clipped_pattern[128], clipped_count;
extern uint8_t sprite_line_lo, sprite_line_hi, sprite_line_ok;
extern const PceScene *video_scene_ptr;
extern uint16_t generic_id;extern int16_t generic_x,generic_y;
extern uint8_t generic_flip,generic_scale,generic_count,generic_slot,generic_ok;
extern void sprite_lines_reserve(void), sprite_lines_release(void);
__attribute__((noinline,section(".ram_bank113.text"))) void sprite_generic(void) {
    uint8_t begin=sat_count,clip_begin=clipped_count;
    uint16_t id=generic_id;int16_t x=generic_x,y=generic_y;bool flip=generic_flip;uint8_t scale=generic_scale,count=generic_count,slot=generic_slot;
    for (uint8_t k = 0; k < count; ++k) {
        const uint8_t *d = descriptor + k * 6;
        int16_t dx = (int16_t)(d[0] | (uint16_t)d[1]<<8);
        int16_t dy = (int16_t)(d[2] | (uint16_t)d[3]<<8);
        if (flip) dx = -dx - 16;
        if (scale != 16) { dx = (int32_t)dx * scale / 16; dy = (int32_t)dy * scale / 16; }
        int16_t px = x + dx, py = y + dy;
        if (px <= -16 || px >= (pce_raster_enabled ? 512 : 256) || py <= -16 || py >= 224) continue;
        /* Cockpit viewing window: the world shows above the consoles (row 158) across the whole width. */
        if (pce_metrics.stage == 6 && id<PCE_MECH_ARM && py >= 158) continue;
        int16_t lo = py < 0 ? 0 : py, hi = py + 16 > 224 ? 224 : py + 16;
        if(sat_count==64)goto refused;
        sprite_line_lo=lo;sprite_line_hi=hi;sprite_lines_reserve();
        if(!sprite_line_ok)goto refused;
        uint16_t pattern = d[4] | (uint16_t)d[5]<<8;
        uint16_t vram_pattern=(sprite_words[slot]>>5)+pattern*2;
        if (pce_metrics.stage==6 && id<PCE_MECH_ARM && (px<0 || px+16>256 || py<0 || py+16>158)) {
            if (clipped_count>=28) { sprite_lines_release();goto refused; }
            uint8_t left=px<0?-px:0, right=px+16>256?256-px:16;
            uint8_t top=py<0?-py:0, bottom=py+16>158?158-py:16;
            if(flip) { uint8_t t=left;left=16-right;right=16-t; }
            uint16_t mask=(0xffffU>>left)&(0xffffU<<(16-right));
            {uint8_t head[4];arcade_read(2,video_scene_ptr->sprites+(uint32_t)id*16,head,4);
            uint32_t pat=(uint32_t)head[0]|(uint32_t)head[1]<<8|(uint32_t)head[2]<<16|(uint32_t)head[3]<<24;
            arcade_read(2,pat+(uint32_t)pattern*128,clipped_pattern,128);}
            for(uint8_t plane=0;plane<4;++plane) for(uint8_t row=0;row<16;++row) {
                uint16_t bits=(row>=top&&row<bottom)?mask:0;
                clipped_pattern[plane*32+row*2]&=bits;
                clipped_pattern[plane*32+row*2+1]&=bits>>8;
            }
            uint16_t word=0x7800+(uint16_t)clipped_count++*64;
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            pce_cpu_irq_disable();pce_vdc_copy_to_vram(word,clipped_pattern,128);pce_cpu_irq_enable();
            pce_metrics.uploads+=128;vram_pattern=word>>5;
        }
        sat[sat_page][sat_count++] = (vdc_sprite_t){py + 64, px + 32,
            vram_pattern,
            VDC_SPRITE_FG | (slot<15?slot:15) | (flip ? VDC_SPRITE_FLIP_X : 0)};
    }
    generic_ok=1;return;
refused:
    while(sat_count>begin) {
        int16_t py=(int16_t)sat[sat_page][--sat_count].y-64;
        sprite_line_lo=py<0?0:py;sprite_line_hi=py+16>224?224:py+16;
        sprite_lines_release();
    }
    clipped_count=clip_begin;{extern uint8_t sprite_optional;if(!sprite_optional)++pce_metrics.essential_overflow;}generic_ok=0;
}
