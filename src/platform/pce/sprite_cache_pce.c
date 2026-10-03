#include "sprite_cache_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
uint16_t sprite_ids[48],sprite_words[48];
uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[48];
uint8_t sprite_slot_of[400],sprite_count[48],sprite_len[48],sprite_p0[48],sprite_p1[48],sprite_p2[48];
uint8_t sprite_pb_lo[48],sprite_pb_hi[48],sprite_attr[48];
/* Resident allocator. A 512-byte page holds four 16x16 patterns. Keep the
 * last two displayed generations pinned through SAT DMA, including palettes.
 * Canonical left/right frames share their cache ID and patterns. */
static uint16_t cache_id;static uint8_t cache_count,cache_result;
#define CACHE_CODE __attribute__((noinline,section(".ram_bank116.text")))
CACHE_CODE static void allocate(void) {
    uint16_t id=cache_id;uint8_t count=cache_count;
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    cache_result=48;
    for(uint8_t i=0;i<48;++i)if(sprite_ids[i]==id){cache_result=i;return;}
    uint16_t first=65535;
    if(s->nforeground)arcade_read(2,s->foreground+4,&first,2);
    uint8_t low=id>=first?15:0,high=id>=first?48:15;
    cache_result=48;
    uint8_t slot=low;
    while(slot<high&&(sprite_used[slot]||sprite_pinned[slot]))++slot;
    if(slot==high)return;
    uint8_t pages=(count+3)>>2;
    for(uint8_t base=0;base<=48-pages;++base) {
        bool available=true;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=pattern_owner[p];
            if(owner&&(sprite_used[owner-1]||sprite_pinned[owner-1])){available=false;break;}
        }
        if(!available)continue;
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
    return;
}

uint8_t sprite_slot(uint16_t id,uint8_t count) {
    cache_id=id;cache_count=count;overlay_call(0x74,allocate);return cache_result;
}
