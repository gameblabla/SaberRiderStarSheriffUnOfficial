#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "assets.h"
#include "sgx_pce.h"
/* Sprite pieces placed one by one (the generic path of video_sprite: a sprite the assembly emitter cannot take). This slow path lives in the $71
 * overlay so the renderer bank keeps room for its hot paths. */
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page, sat_count, descriptor[384], clipped_pattern[128], clipped_count;
extern uint8_t sprite_line_lo, sprite_line_hi, sprite_line_ok;
extern const PceScene *video_scene_ptr;
extern uint16_t generic_id;extern int16_t generic_x,generic_y;
extern uint8_t generic_flip,generic_scale,generic_count,generic_slot,generic_ok;
extern void sprite_lines_reserve(void), sprite_lines_release(void);
__attribute__((noinline,section(".ram_bank116.text"))) void sprite_generic(void) {
    uint8_t begin=sat_count,clip_begin=clipped_count;
    int16_t x=generic_x,y=generic_y;bool flip=generic_flip;uint8_t count=generic_count,slot=generic_slot;
    for (uint8_t k = 0; k < count; ++k) {
        const uint8_t *d = descriptor + k * 6;
        uint16_t flags=d[4]|(uint16_t)d[5]<<8;
        if(flags&0x4000)continue;
        uint8_t width=flags&0x8000?32:16;
        int16_t dx = (int16_t)(d[0] | (uint16_t)d[1]<<8);
        int16_t dy = (int16_t)(d[2] | (uint16_t)d[3]<<8);
        if (flip) dx = -dx - width;
        int16_t px = x + dx, py = y + dy;
        if (px <= -(int16_t)width || px >= (pce_raster_enabled ? 512 : 256) || py <= -16 || py >= 224) continue;
        int16_t lo = py < 0 ? 0 : py, hi = py + 16 > 224 ? 224 : py + 16;
        if(sat_count==64)goto refused;
        sprite_line_lo=lo;sprite_line_hi=hi;sprite_lines_reserve();
        if(!sprite_line_ok)goto refused;
        if(width==32) {
            sprite_lines_reserve();
            if(!sprite_line_ok){sprite_lines_release();goto refused;}
        }
        uint16_t pattern=flags&0x3fff;
        uint16_t word=sprite_words[slot];
#ifdef PCE_SGX
        if(sat_page==1&&pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2)word=sprite_words1[slot];
#endif
        uint16_t vram_pattern=(word>>5)+pattern*2;
        sat[sat_page][sat_count++] = (vdc_sprite_t){py + 64, px + 32,
            vram_pattern,
            sprite_attr[slot] | (flip ? VDC_SPRITE_FLIP_X : 0) | (width==32?VDC_SPRITE_WIDTH_32:0)};
    }
    if(sat_count>begin)sprite_used[slot]=1;
    generic_ok=1;return;
refused:
    while(sat_count>begin) {
        int16_t py=(int16_t)sat[sat_page][--sat_count].y-64;
        sprite_line_lo=py<0?0:py;sprite_line_hi=py+16>224?224:py+16;
        sprite_lines_release();
        if(sat[sat_page][sat_count].attr&VDC_SPRITE_WIDTH_32)sprite_lines_release();
    }
    clipped_count=clip_begin;{extern uint8_t sprite_optional;if(!sprite_optional)++pce_metrics.essential_overflow;}generic_ok=0;
}

#ifdef PCE_SGX
/* Cache misses must pass the same whole-object admission before allocating
 * or uploading patterns. Roll back the probe so the real emitter can commit
 * the identical geometry after the upload. IRQs never change these budgets. */
extern uint8_t sprite_screen_height,sprite_last_free,sprite_last_lo;
static uint8_t probe_lo[32] __attribute__((section(".ram_bank116.probe")));
static uint8_t probe_units[32] __attribute__((section(".ram_bank116.probe")));
static uint8_t probe_hi[32] __attribute__((section(".ram_bank116.probe")));
__attribute__((noinline,section(".ram_bank116.text"))) void sprite_probe(void) {
    extern uint16_t sprite_emit_id,video_nsprites;
    extern int16_t sprite_emit_x,sprite_emit_y;
    extern uint8_t sprite_emit_flip,sprite_fast_miss,sprite_emit_ok;
    if(!pce_sgx_gameplay()||sprite_emit_id>=video_nsprites)return;
    uint8_t entry[16],slot=sprite_slot_of[sprite_emit_id];
    bool cached=slot<48&&sprite_ids[slot]==sprite_emit_id;
    uint32_t pieces;
    if(cached) {
        generic_count=sprite_count[slot];
        pieces=(uint32_t)sprite_p0[slot]|(uint32_t)sprite_p1[slot]<<8|(uint32_t)sprite_p2[slot]<<16;
    } else {
        arcade_read(2,video_scene_ptr->sprites+(uint32_t)sprite_emit_id*16,entry,16);
        generic_count=entry[12];
        if(!generic_count||generic_count>32||entry[13]){sprite_fast_miss=0;return;}
        pieces=(uint32_t)entry[4]|(uint32_t)entry[5]<<8|(uint32_t)entry[6]<<16;
    }
    arcade_read(2,pieces,descriptor,generic_count*6);
    generic_x=sprite_emit_x;generic_y=sprite_emit_y;generic_flip=sprite_emit_flip!=0;
    uint8_t n=0,last=sprite_last_free,last_lo=sprite_last_lo;
    generic_ok=1;
    for(uint8_t k=0;k<generic_count;++k) {
        const uint8_t *d=descriptor+k*6;
        uint16_t flags=d[4]|(uint16_t)d[5]<<8;
        if(flags&0x4000)continue;
        uint8_t width=flags&0x8000?32:16;
        int16_t dx=(int16_t)(d[0]|(uint16_t)d[1]<<8);
        int16_t dy=(int16_t)(d[2]|(uint16_t)d[3]<<8);
        int16_t x=generic_x+(generic_flip?-dx-width:dx),y=generic_y+dy;
        if(x<=-(int16_t)width||x>=(pce_raster_enabled?512:256)||y<=-16||y>=sprite_screen_height)continue;
        if(sat_count+n==64){generic_ok=0;break;}
        sprite_line_lo=y<0?0:y;
        sprite_line_hi=y+16>sprite_screen_height?sprite_screen_height:y+16;
        sprite_lines_reserve();
        if(!sprite_line_ok){generic_ok=0;break;}
        if(width==32) {
            sprite_lines_reserve();
            if(!sprite_line_ok){sprite_lines_release();generic_ok=0;break;}
        }
        probe_units[n]=width==32?2:1;
        probe_lo[n]=sprite_line_lo;probe_hi[n]=sprite_line_hi;++n;
    }
    if(generic_ok&&!n)generic_ok=2;
    while(n) {
        --n;sprite_line_lo=probe_lo[n];sprite_line_hi=probe_hi[n];sprite_lines_release();
        if(probe_units[n]==2)sprite_lines_release();
    }
    sprite_last_free=last;sprite_last_lo=last_lo;
    if(generic_ok!=1) {
        sprite_fast_miss=0;sprite_emit_ok=generic_ok==2;
    }
}
#endif
