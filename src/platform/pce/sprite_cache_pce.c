#include "sprite_cache_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
uint16_t sprite_ids[48],sprite_words[48];
uint8_t sprite_stamp[48],sprite_epoch;
uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[54];
uint8_t sprite_slot_of[480],sprite_count[48],sprite_len[48],sprite_p0[48],sprite_p1[48],sprite_p2[48];
uint8_t sprite_pb_hi[48],sprite_attr[48];
/* Resident allocator. A 512-byte page holds four 16x16 patterns. Keep the
 * last two displayed generations pinned through SAT DMA, including palettes.
 * Canonical left/right frames share their cache ID and patterns. */
static uint16_t cache_id,cache_count,cache_result;
uint16_t sprite_cache_foreground_first;
uint8_t sprite_cache_stage;
#define CACHE_CODE __attribute__((noinline,minsize,section(".ram_bank116.text")))
CACHE_CODE static void allocate(void) {
    uint16_t id=cache_id;uint8_t count=cache_count;
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    cache_result=48;
    uint8_t known=sprite_slot_of[id];
    if(known<48&&sprite_ids[known]==id){cache_result=known;return;}
    if(sprite_cache_stage!=pce_metrics.stage) {
        sprite_cache_foreground_first=65535;
        if(s->nforeground)arcade_read(2,s->foreground+4,&sprite_cache_foreground_first,2);
        else if(pce_hud_base[pce_metrics.stage-1])sprite_cache_foreground_first=pce_hud_base[pce_metrics.stage-1];   /* the HUD pieces share one palette */
        sprite_cache_stage=pce_metrics.stage;
    }
    /* Ramrod's arena keeps palettes 29 (the arm) and 30 (the big mech) (m6_d.c) and the top 16 pages of the cache for one of the mech's two pattern buffers */
    bool arena=pce_metrics.stage==6;
    uint8_t low=id>=sprite_cache_foreground_first?15:0,high=id>=sprite_cache_foreground_first?48:arena?13:15;
    cache_result=48;
    uint8_t slot=high;
    for(uint8_t i=low;i<high;++i)
        if(!sprite_used[i]&&!sprite_pinned[i]&&sprite_ids[i]==0xffff){slot=i;break;}
    if(slot==high) {
        uint8_t oldest=0;
        for(uint8_t i=low;i<high;++i)if(!sprite_used[i]&&!sprite_pinned[i]) {
            uint8_t age=sprite_epoch-sprite_stamp[i];
            if(slot==high||age>oldest){slot=i;oldest=age;}
        }
    }
    if(slot==high)return;
    /* Prefer empty pages, then the block whose youngest owner is oldest.
     * Live and pinned owners are protected in either case. */
    uint8_t pages=(count+3)>>2,chosen=255;
    uint8_t best_age=0;
    uint8_t limit=(herd_on?28:arena?32:48)-pages;
    /* Platform HUDs need two four-page generations. Keep their blocks
     * contiguous even when a boss reserves pages 16..39; actors and scenery
     * must not fragment the space needed by the next health/lives update. */
    uint16_t hud=pce_present_base[pce_metrics.stage-1][0];
    bool icon=hud&&id>=hud&&id<hud+16;
    /* Platform play does not use the arena's dedicated $7800-$7dff HUD.
     * Use those six pages too, stopping before the alternate SAT at $7e00. */
    if(hud&&!herd_on)limit=54-pages;
    uint8_t first=hud?14:pce_metrics.stage==2?6:0,step=1;
    if(icon){first=0;limit=4;step=4;}
    else if(hud) {
        uint16_t motion=pce_motion_base[pce_metrics.stage-1];
        if(id<36||(id>=hud+26&&id<hud+90)||(id>=motion&&id<motion+67)) {
            /* Three two-page hero poses cover the active SAT and its two
             * protected generations, including a return from invulnerability. */
            first=8;limit=12;step=2;
        } else if(id>=hud+16&&id<hud+26) {first=48;limit=50;}
    }
    /* Race dialogue glyphs extend through $4dff; leave those six pages
     * reserved throughout the race, including before a panel opens. */
    for(uint8_t base=first;base<=limit;base+=step) {
        if(hud&&first==14&&base<=50&&base+pages>48)continue;
        bool available=true;uint8_t age=255;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=pattern_owner[p];
            if(!owner||owner==slot+1)continue;
            if(sprite_used[owner-1]||sprite_pinned[owner-1]){available=false;break;}
            uint8_t elapsed=sprite_epoch-sprite_stamp[owner-1];
            if(elapsed<age)age=elapsed;
        }
        if(available&&(chosen==255||age>best_age)){chosen=base;best_age=age;if(age==255)break;}
    }
    if(chosen==255)return;
    uint8_t base=chosen;
    for(uint8_t p=base;p<base+pages;++p) {
        uint8_t owner=pattern_owner[p];
        if(owner) {
            sprite_ids[owner-1]=0xffff;
            for(uint8_t q=0;q<54;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
        }
    }
    for(uint8_t q=0;q<54;++q)if(pattern_owner[q]==slot+1)pattern_owner[q]=0;
    for(uint8_t p=base;p<base+pages;++p)pattern_owner[p]=slot+1;
    sprite_words[slot]=PCE_SPR_WORD+(uint16_t)base*256;
    cache_result=slot;return;
}

uint8_t sprite_slot(uint16_t id,uint8_t count) {
    cache_id=id;cache_count=count;overlay_call(0x74,allocate);return cache_result;
}
