#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#include <string.h>

#ifdef PCE_SGX
#define SGX_SPLIT_CODE __attribute__((noinline,minsize,section(".ram_bank129.text")))

extern uint8_t buffer[2048];
extern uint8_t actors_mode;
extern uint8_t sat_count,sat_page,sprite_occupancy[240],sprite_last_free;
extern vdc_sprite_t sat[2][64];
extern void sprite_lines_clear(void);

static SGX_SPLIT_CODE void split_sat_to_end(uint8_t first,uint8_t end) {
    if(end<=first||sat_count<=end)return;
    uint8_t n=end-first,m=sat_count-end;
    uint8_t *at=(uint8_t *)(sat[1]+first),*held=buffer+1024;
    memcpy(held,at,(uint16_t)n*8);
    for(uint8_t k=0;k<m;++k)memcpy(at+(uint16_t)k*8,at+(uint16_t)(k+n)*8,8);
    memcpy(at+(uint16_t)m*8,held,(uint16_t)n*8);
}

/* Keep the player, HUD and foreground on VDC0. Draw the remaining platform
   actors on VDC1, whose sprite budget and cache residency are independent. */
SGX_SPLIT_CODE void pce_sgx_platform_actor_pass_body(void) {
    if(sat_page!=0)return;
    uint8_t vdc0_count=sat_count,vdc0_last_free=sprite_last_free;
    memcpy(buffer+256,sprite_occupancy,240);
    sat_page=1;sat_count=0;sprite_lines_clear();

    actors_mode=1;
    overlay_call(0x74,actors_draw);
    uint8_t props_end=sat_count;
    uint8_t hull_first=sat_count;
    if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
    uint8_t hull_end=sat_count;
    if(pce_campaign.boss_kind==2) {
        pce_control.phase=0;
        overlay_call(0x7c,shots_draw_pass);
    }
    actors_mode=2;
    overlay_call(0x74,actors_draw);
    pce_control.phase=1;
    overlay_call(0x7c,shots_draw_pass);
    actors_mode=0;

    if(pce_campaign.boss_kind==2)split_sat_to_end(hull_first,hull_end);
    split_sat_to_end(0,props_end);
    uint8_t shake=(herd_on?((frame*13^(frame>>2))&3):0);
    if(shake)for(uint8_t k=0;k<sat_count;++k)sat[1][k].y-=shake;
    for(uint8_t k=sat_count;k<64;++k)sat[1][k].y=0;
    overlay_call(0x71,pce_sgx_vdc1_stats_body);
    overlay_call(0x78,pce_sgx_vdc1_sat_upload_body);

    sat_page=0;sat_count=vdc0_count;
    memcpy(sprite_occupancy,buffer+256,240);
    sprite_last_free=vdc0_last_free;
}
#endif
