#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "assets.h"
/* Sprite pieces placed one by one (the generic path of video_sprite: a sprite the assembly emitter cannot take). This slow path lives in the $71
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
    int16_t x=generic_x,y=generic_y;bool flip=generic_flip;uint8_t count=generic_count,slot=generic_slot;
    for (uint8_t k = 0; k < count; ++k) {
        const uint8_t *d = descriptor + k * 6;
        int16_t dx = (int16_t)(d[0] | (uint16_t)d[1]<<8);
        int16_t dy = (int16_t)(d[2] | (uint16_t)d[3]<<8);
        if (flip) dx = -dx - 16;
        int16_t px = x + dx, py = y + dy;
        if (px <= -16 || px >= (pce_raster_enabled ? 512 : 256) || py <= -16 || py >= 224) continue;
        int16_t lo = py < 0 ? 0 : py, hi = py + 16 > 224 ? 224 : py + 16;
        if(sat_count==64)goto refused;
        sprite_line_lo=lo;sprite_line_hi=hi;sprite_lines_reserve();
        if(!sprite_line_ok)goto refused;
        uint16_t pattern = d[4] | (uint16_t)d[5]<<8;
        uint16_t vram_pattern=(sprite_words[slot]>>5)+pattern*2;
        sat[sat_page][sat_count++] = (vdc_sprite_t){py + 64, px + 32,
            vram_pattern,
            sprite_attr[slot] | (flip ? VDC_SPRITE_FLIP_X : 0)};
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
