#include "sprite_cache_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
uint16_t sprite_ids[48],sprite_words[48];
uint8_t sprite_stamp[48],sprite_epoch;
uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[48];
uint8_t sprite_slot_of[480],sprite_count[48],sprite_len[48],sprite_p0[48],sprite_p1[48],sprite_p2[48];
uint8_t sprite_pb_lo[48],sprite_pb_hi[48],sprite_attr[48];
/* Resident allocator. A 512-byte page holds four 16x16 patterns. Keep the
 * last two displayed generations pinned through SAT DMA, including palettes.
 * Canonical left/right frames share their cache ID and patterns. */
static uint16_t cache_id,cache_first;static uint8_t cache_count,cache_result,cache_stage;
#define CACHE_CODE __attribute__((noinline,section(".ram_bank116.text")))
CACHE_CODE static void allocate(void) {
    uint16_t id=cache_id;uint8_t count=cache_count;
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    cache_result=48;
    uint8_t known=sprite_slot_of[id];
    if(known<48&&sprite_ids[known]==id){cache_result=known;return;}
    if(cache_stage!=pce_metrics.stage) {
        cache_first=65535;
        if(s->nforeground)arcade_read(2,s->foreground+4,&cache_first,2);
        cache_stage=pce_metrics.stage;
    }
    uint8_t low=id>=cache_first?15:0,high=id>=cache_first?48:15;
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
    uint8_t pages=(count+3)>>2,chosen=48;
    uint8_t best_age=0;
    uint8_t limit=(herd_on?28:48)-pages;
    for(uint8_t base=0;base<=limit;++base) {
        bool available=true;uint8_t age=255;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=pattern_owner[p];
            if(!owner||owner==slot+1)continue;
            if(sprite_used[owner-1]||sprite_pinned[owner-1]){available=false;break;}
            uint8_t elapsed=sprite_epoch-sprite_stamp[owner-1];
            if(elapsed<age)age=elapsed;
        }
        if(available&&(chosen==48||age>best_age)){chosen=base;best_age=age;if(age==255)break;}
    }
    if(chosen==48)return;
    uint8_t base=chosen;
    for(uint8_t p=base;p<base+pages;++p) {
        uint8_t owner=pattern_owner[p];
        if(owner) {
            sprite_ids[owner-1]=0xffff;
            for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
        }
    }
    for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==slot+1)pattern_owner[q]=0;
    for(uint8_t p=base;p<base+pages;++p)pattern_owner[p]=slot+1;
    sprite_words[slot]=PCE_SPR_WORD+(uint16_t)base*256;
    cache_result=slot;return;
}

uint8_t sprite_slot(uint16_t id,uint8_t count) {
    cache_id=id;cache_count=count;overlay_call(0x74,allocate);return cache_result;
}
