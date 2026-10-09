#include "play_internal.h"
#include "presentation_pce.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#include "sprite_cache_pce.h"
#include <string.h>

#ifdef PCE_SGX
#define SGX_SPLIT_CODE __attribute__((noinline,minsize,section(".ram_bank114.text")))
#define SGX_SPLIT_HELPER __attribute__((noinline,minsize,section(".ram_bank117.text")))

extern uint8_t buffer[2048];
extern uint8_t actors_mode;
extern uint8_t sprite_exact,sprite_lines_hi,sat_page,sprite_occupancy[240];
extern void sprite_lines_select(void);
__attribute__((noinline,minsize,section(".ram_bank110.text")))
void pce_sgx_budget_body(void) {
    sprite_lines_hi=(uint16_t)(sat_page?buffer+512:sprite_occupancy)>>8;
    sprite_lines_select();
}
extern uint8_t sat_count,sat_page,sprite_occupancy[240],sprite_last_free,sprite_last_lo;

extern vdc_sprite_t sat[2][64];
extern void sprite_lines_clear(void);
extern uint16_t hud_copy_src,hud_copy_dst,hud_copy_len;
extern uint8_t hud_copy_opcode;
extern void hud_copy(void);
/* Short hardware block transfers leave the PCM IRQ serviceable. This also
   supports forward overlapping moves when dst precedes src. */
__attribute__((noinline,minsize,section(".ram_bank106.text")))
void pce_sgx_copy(void *dst,const void *src,uint16_t bytes) {
    if(!bytes)return;
    hud_copy_src=(uint16_t)src;hud_copy_dst=(uint16_t)dst;
    hud_copy_len=bytes;hud_copy_opcode=0x73;hud_copy();
}
extern void sprite_lines_release(void),sprite_lines_reserve(void);
extern uint8_t sprite_line_lo,sprite_line_hi;
uint8_t pce_sgx_hero_first PCE_WORK;
uint8_t pce_sgx_hero_visible;
uint8_t pce_sgx_split_active PCE_WORK,pce_sgx_split_count PCE_WORK,pce_sgx_split_last PCE_WORK,pce_sgx_split_last_lo PCE_WORK;
uint8_t pce_sgx_actor_plane[8] PCE_WORK,pce_sgx_actor_stage PCE_WORK;

/* The front plane's entire scanline budget belongs to the foreground. Move
   the hero to the beginning of the world SAT before admitting other actors. */
__attribute__((noinline,minsize,section(".ram_bank128.text")))
static void platform_begin_body(void) {
    uint8_t first=pce_sgx_hero_first;
    uint8_t count=sat_count-first;
    pce_sgx_copy(sat[1],sat[0]+first,(uint16_t)count*8);
    while(sat_count>first) {
        int16_t y=(int16_t)sat[0][--sat_count].y-64;
        sprite_line_lo=y<0?0:y;sprite_line_hi=y+16>224?224:y+16;
        sprite_lines_release();
        if(sat[0][sat_count].attr&VDC_SPRITE_WIDTH_32)sprite_lines_release();
    }
    /* Reserve the foreground's VDC0 SAT and scanlines before actors/shots
       retry there. Otherwise firing steals those slots and makes retained
       scenery blink. This prefix is restored after the VDC1 pass. */
    overlay_call(0x75,foreground_draw);
    pce_sgx_split_count=sat_count;pce_sgx_split_last=sprite_last_free;pce_sgx_split_last_lo=sprite_last_lo;pce_sgx_split_active=1;
    sat_page=1;sat_count=count;
    overlay_call(0x6e,pce_sgx_budget_body);sprite_lines_clear();
    if(pce_sgx_hero_visible)video_sprite(hero_sprite,player.x-camera,player.y-16,facing,16);
    if(count) {
        uint8_t slot=sprite_slot_of[hero_sprite];
        if(!(sprite_pb_hi[slot]&0x80)) {
            pce_sgx_sprite_id=hero_sprite;pce_sgx_sprite_slot=slot;
            overlay_call(0x80,pce_sgx_cache_upload_body);
            if(pce_sgx_sprite_upload_ok)sprite_pb_hi[slot]|=0x80;
        }
        if(!(sprite_pb_hi[slot]&0x80))sat_count=count=0;
        int16_t shift=(int16_t)(sprite_words1[slot]-sprite_words[slot])/32;
        for(uint8_t k=0;k<count;++k) {
            sat[1][k].pattern+=shift;
            int16_t y=(int16_t)sat[1][k].y-64;
            sprite_line_lo=y<0?0:y;sprite_line_hi=y+16>224?224:y+16;
            sprite_lines_reserve();
            if(sat[1][k].attr&VDC_SPRITE_WIDTH_32)sprite_lines_reserve();
        }
    }
}

static uint8_t split_first PCE_WORK,split_end PCE_WORK;
SGX_SPLIT_HELPER static void split_sat_to_end_body(void) {
    uint8_t first=split_first,end=split_end;
    uint8_t n=end-first,m=sat_count-end;
    uint8_t *at=(uint8_t *)(sat[1]+first),*held=buffer+1024;
    pce_sgx_copy(held,at,(uint16_t)n*8);
    pce_sgx_copy(at,at+(uint16_t)n*8,(uint16_t)m*8);
    pce_sgx_copy(at+(uint16_t)m*8,held,(uint16_t)n*8);
}
static SGX_SPLIT_CODE void split_sat_to_end(uint8_t first,uint8_t end) {
    if(end<=first||sat_count<=end)return;
    split_first=first;split_end=end;
    overlay_call(0x75,split_sat_to_end_body);
}

__attribute__((noinline,minsize,section(".ram_bank128.text")))
void pce_sgx_story_world_body(void) {
    pce_sgx_hero_visible=1;pce_sgx_hero_first=sat_count;
    overlay_call(0x74,foreground_prepare);
    overlay_call(0x72,pce_sgx_platform_actor_pass_body);
}

/* Keep HUD and foreground on VDC0, and draw the platform actors on VDC1. */
SGX_SPLIT_CODE void pce_sgx_platform_actor_pass_body(void) {
    if(sat_page!=0)return;
    if(pce_sgx_actor_stage!=pce_metrics.stage) {
        memset(pce_sgx_actor_plane,0xff,sizeof pce_sgx_actor_plane);
        pce_sgx_actor_stage=pce_metrics.stage;
    }
    overlay_call(0x80,platform_begin_body);
    uint8_t fg_first=sat_count;

    /* The stampede used to monopolize VDC0's scanlines and made its source
       foreground disappear. Give the horses VDC1's independent SAT/budget. */
    if(herd_on&&(pce_metrics.stage==1||pce_metrics.stage==3))
        overlay_call(0x6f,herd_draw);

    actors_mode=1;
    overlay_call(0x77,actors_draw);
    uint8_t props_end=sat_count;
    uint8_t hull_first=sat_count;
    if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
    uint8_t hull_end=sat_count;
    if(pce_campaign.boss_kind) {
        pce_control.phase=0;
        overlay_call(0x7c,shots_draw_pass);
    }
    actors_mode=2;
    overlay_call(0x77,actors_draw);
    pce_control.phase=1;
    overlay_call(0x7c,shots_draw_pass);
    actors_mode=0;

    if(pce_campaign.boss_kind==2)split_sat_to_end(hull_first,hull_end);
    split_sat_to_end(fg_first,props_end);
    uint8_t shake=(herd_on?((frame*13^(frame>>2))&3):0);
    if(shake)for(uint8_t k=0;k<sat_count;++k)sat[1][k].y-=shake;
    overlay_call(0x71,pce_sgx_vdc1_stats_body);
    overlay_call(0x78,pce_sgx_vdc1_sat_upload_body);

    pce_sgx_split_active=0;
    sat_page=0;sat_count=pce_sgx_split_count;sprite_last_free=pce_sgx_split_last;sprite_last_lo=pce_sgx_split_last_lo;
    overlay_call(0x6e,pce_sgx_budget_body);
}
#endif
