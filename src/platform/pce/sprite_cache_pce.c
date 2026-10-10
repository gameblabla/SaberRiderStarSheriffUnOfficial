#include "sprite_cache_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#include "campaign_pce.h"
extern uint8_t sat_page;
uint16_t sprite_ids[48],sprite_words[48];
uint8_t sprite_stamp[48],sprite_epoch;
uint8_t sprite_used[48],sprite_pinned[48],pattern_owner[54];
uint8_t sprite_slot_of[PCE_SPRITE_IDS],sprite_count[48],sprite_len[48],sprite_p0[48],sprite_p1[48],sprite_p2[48];
#ifdef PCE_SGX
/* Bank $68 stays mapped at $4000 throughout every overlay and stage load.
 * Only platform scenes use the independent VDC1 pattern directory. Palette
 * slots, descriptors and generation pins remain shared through the VCE. */
uint16_t sprite_words1[48] __attribute__((section(".ram_bank104.cache")));
uint8_t pattern_owner1[54];
uint32_t sprite_requested_palette;
uint32_t sprite_palette_key[16] __attribute__((section(".ram_bank129.palette")));
uint8_t sprite_palette_needs_upload;
static uint8_t palette_slot;
static uint8_t palette_result;
__attribute__((noinline,section(".ram_bank129.text")))
void sprite_palette_reset(void) {
    for(uint8_t p=0;p<16;++p)sprite_palette_key[p]=0;
}
/* Pattern slots are independent of the VCE's twelve actor palettes. Exact
 * archive palette identity lets all poses retain the same hardware colors. */
__attribute__((noinline,section(".ram_bank129.text")))
static void palette_prepare(void) {
    uint16_t protected=0;
    palette_result=255;sprite_palette_needs_upload=1;
    /* VCE palettes are shared by all poses and both VDCs. Search twelve
       hardware identities once, rather than comparing up to 48 pose keys.
       Retiring a pattern does not change its palette's VCE contents. */
    for(uint8_t pal=0;pal<12;++pal)
        if(sprite_palette_key[pal]==sprite_requested_palette) {
            palette_result=pal;sprite_palette_needs_upload=0;return;
        }
    for(uint8_t q=0;q<48;++q) {
        if(sprite_ids[q]==65535)continue;
        uint8_t pal=sprite_attr[q]&15;
        if(pal>=12)continue;
        if(sprite_used[q]||sprite_pinned[q])protected|=(uint16_t)1<<pal;
    }
    for(uint8_t pal=0;pal<12;++pal)if(!(protected&((uint16_t)1<<pal))) {
        palette_result=pal;
        for(uint8_t q=0;q<48;++q)if(q!=palette_slot && (sprite_attr[q]&15)==pal) {
            sprite_ids[q]=65535;sprite_pb_hi[q]=0;
        }
        return;
    }
}

uint8_t sprite_pb_hi[48] PCE_WORK,sprite_attr[48] PCE_WORK;
#else
uint8_t sprite_pb_hi[48],sprite_attr[48];
#endif
/* Resident allocator. A 512-byte page holds four 16x16 patterns. Keep the
 * last two displayed generations pinned through SAT DMA, including palettes.
 * Canonical left/right frames share their cache ID and patterns. */
static uint16_t cache_id,cache_count,cache_result;
uint16_t sprite_cache_foreground_first;
uint8_t sprite_cache_stage;
__attribute__((noinline,minsize,section(".ram_bank107.text")))
uint8_t sprite_palette(uint16_t id,uint8_t slot) {
#ifdef PCE_SGX
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2)
        return id>=sprite_cache_foreground_first?12:sprite_attr[slot]&15;
#else
    (void)id;
#endif
    return slot<15?slot:15;
}
#define CACHE_CODE __attribute__((noinline,section(".ram_bank129.text")))
#ifdef PCE_SGX
CACHE_CODE static void pages_release(uint8_t *owners,uint16_t *words,uint8_t slot) {
    uint8_t owner=slot+1;
    /* Owner 48 also marks horse/hull reservations, which need not be
       described by slot 47's ordinary pose metadata. */
    if(slot==47) {
        for(uint8_t p=0;p<54;++p)if(owners[p]==owner)owners[p]=0;
        return;
    }
    /* Each ordinary allocation is one contiguous block per VDC. Keep the
       ownership check: part of an old block may already be retired. */
    uint8_t first=(words[slot]>>8)-(PCE_SPR_WORD>>8);
    if(first>=54)return;
    uint8_t end=first+((sprite_count[slot]+3)>>2);
    for(uint8_t p=first;p<end;++p)if(owners[p]==owner)owners[p]=0;
}
#endif
CACHE_CODE static void allocate(void) {
    uint16_t id=cache_id;uint8_t count=cache_count;
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    cache_result=48;
    uint8_t known=sprite_slot_of[id];
#ifdef PCE_SGX
    bool split=pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2;
    bool second=split&&sat_page==1;
    uint8_t bit=second?0x80:0x40;
    uint8_t *owners=second?pattern_owner1:pattern_owner;
    uint16_t *words=second?sprite_words1:sprite_words;
    bool cached=known<48&&sprite_ids[known]==id;
    if(cached&&(!split||(sprite_pb_hi[known]&bit))){cache_result=known;return;}
#else
    uint8_t *owners=pattern_owner;
    uint16_t *words=sprite_words;
    if(known<48&&sprite_ids[known]==id){cache_result=known;return;}
#endif
    if(sprite_cache_stage!=pce_metrics.stage) {
        sprite_cache_foreground_first=65535;
        if(s->nforeground)arcade_read(2,s->foreground+4,&sprite_cache_foreground_first,2);
        else if(pce_hud_base[pce_metrics.stage-1])sprite_cache_foreground_first=pce_hud_base[pce_metrics.stage-1];   /* the HUD pieces share one palette */
        sprite_cache_stage=pce_metrics.stage;
    }
    /* Ramrod's arena keeps palettes 29 (the arm) and 30 (the big mech) (m6_d.c) and the top 16 pages of the cache for one of the mech's two pattern buffers */
    bool arena=pce_metrics.stage==6;
    uint16_t hud=pce_present_base[pce_metrics.stage-1][0];
    uint8_t low=id>=sprite_cache_foreground_first?15:0,high=id>=sprite_cache_foreground_first?48:arena?13:15;
#ifdef PCE_SGX
    if(pce_sgx_gameplay()&&hud&&id>=hud&&id<hud+26){low=15;high=48;}
    /* BG/HUD, foreground and herd have different palettes even though the
       VDCs share one VCE. Reserve palettes 12 (foreground), 13 (herd), 14 (hull). */
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2&&low==0)
        high=12;
#endif
    cache_result=48;
    uint8_t slot;
#ifdef PCE_SGX
    if(split&&low==0)high=48;
    slot=high;
    if(cached)slot=known;
    else
#else
    slot=high;
#endif
    {
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
    }
    /* Prefer empty pages, then the block whose youngest owner is oldest.
     * Live and pinned owners are protected in either case. */
    uint8_t pages=(count+3)>>2,chosen=255;
    uint8_t best_age=0;
    uint8_t limit=(herd_on?28:arena?32:48)-pages;
    /* Platform HUDs need two four-page generations. Keep their blocks
     * contiguous even when a boss reserves pages 16..39; actors and scenery
     * must not fragment the space needed by the next health/lives update. */
    bool icon=hud&&id>=hud&&id<hud+16;
    /* Platform play does not use the arena's dedicated $7800-$7dff HUD.
     * Use those six pages too, stopping before the alternate SAT at $7e00. */
    if(hud&&!herd_on)limit=54-pages;
#ifdef PCE_SGX
    /* Space panels need six contiguous pages. The last six cache pages are
       free in space (the arena's fixed HUD is absent), even when both world
       SAT generations still pin fragmented blocks below $7800. */
    uint16_t dialog=pce_dialog_base[6];
    if(pce_sgx_gameplay()&&pce_metrics.stage==7&&id>=dialog&&id<dialog+8)
        limit=54-pages;
    if(hud&&herd_on&&pce_sgx_gameplay()&&id>=sprite_cache_foreground_first)limit=54-pages;
    /* Protect VDC1's resident horses through all convoys. The subsequent
       boss owns overlapping storage, so release the horse restriction then.
       VDC0 foreground/HUD may use these addresses throughout the scene. */
    if(second&&s->horse&&!pce_campaign.boss_kind&&limit>28-pages)
        limit=28-pages;
#endif
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
#ifdef PCE_SGX
    if(second&&hud&&first==14)first=0;
    else if(split&&hud&&first==14&&id<sprite_cache_foreground_first)first=8;
    /* The split platform hero lives on VDC1. VDC0 needs only the HUD's
     * pages 0..7; its unused hero range must remain available to foreground
     * chunks beside the two reserved horse buffers. Live fallback actors
     * are still protected by the normal page-owner/generation checks. */
    if(split&&!second&&hud&&id>=sprite_cache_foreground_first)first=8;
#endif
    /* Race dialogue glyphs extend through $4dff; leave those six pages
     * reserved throughout the race, including before a panel opens. */
    for(uint8_t base=first;base<=limit;base+=step) {
#ifdef PCE_SGX
        if(second&&hud&&first==0&&base<14&&base+pages>8)continue;
#endif
        if(hud&&first!=48&&base<=50&&base+pages>48)continue;
        bool available=true;uint8_t age=255;
        for(uint8_t p=base;p<base+pages;++p) {
            uint8_t owner=owners[p];
            if(!owner||owner==slot+1)continue;
            if(sprite_used[owner-1]||sprite_pinned[owner-1]){
                /* Every candidate starting before this protected page also
                 * overlaps it. Skip those repeated scans of the same block. */
                if(step==1)base=p;
                available=false;break;
            }
            uint8_t elapsed=sprite_epoch-sprite_stamp[owner-1];
#ifdef PCE_SGX
            /* Its youngest owner already makes this candidate no better
             * than the saved block. The remaining pages cannot raise that
             * minimum, so preserve the same LRU choice without scanning them. */
            if(chosen!=255&&elapsed<=best_age){available=false;break;}
#endif
            if(elapsed<age)age=elapsed;
        }
        if(available&&(chosen==255||age>best_age)){chosen=base;best_age=age;if(age==255)break;}
    }
    if(chosen==255)return;
    uint8_t base=chosen;
#ifdef PCE_SGX
    if(split&&!cached) {
        if(id>=sprite_cache_foreground_first)palette_result=12;
        else if(hud&&id>=hud&&id<hud+26)palette_result=15;
        else {
            palette_slot=slot;palette_prepare();
            if(palette_result==255)return;
        }
        if(palette_result>=12) {
            if(sprite_palette_key[palette_result]==sprite_requested_palette)sprite_palette_needs_upload=0;
        }
        sprite_attr[slot]=palette_result;
        sprite_palette_key[palette_result]=sprite_requested_palette;
    }

    if(split&&!cached) {
        /* Reusing a palette/descriptor slot retires its allocations on both
           VDCs. The shared pins above protect either displayed generation. */
        pages_release(pattern_owner,sprite_words,slot);
        pages_release(pattern_owner1,sprite_words1,slot);
        sprite_pb_hi[slot]=0;
    }
#endif
    for(uint8_t p=base;p<base+pages;++p) {
        uint8_t owner=owners[p];
        if(owner) {
#ifdef PCE_SGX
            if(split) {
                sprite_pb_hi[owner-1]&=~bit;
                if(!sprite_pb_hi[owner-1])sprite_ids[owner-1]=0xffff;
            } else
#endif
            sprite_ids[owner-1]=0xffff;
#ifdef PCE_SGX
            pages_release(owners,words,owner-1);
#else
            for(uint8_t q=0;q<54;++q)if(owners[q]==owner)owners[q]=0;
#endif
        }
    }
#ifdef PCE_SGX
    pages_release(owners,words,slot);
#else
    for(uint8_t q=0;q<54;++q)if(owners[q]==slot+1)owners[q]=0;
#endif
    for(uint8_t p=base;p<base+pages;++p)owners[p]=slot+1;
    words[slot]=PCE_SPR_WORD+(uint16_t)base*256;
    cache_result=slot;return;
}

uint8_t sprite_slot(uint16_t id,uint8_t count) {
    cache_id=id;cache_count=count;overlay_call(0x81,allocate);return cache_result;
}

#ifdef PCE_SGX
__attribute__((noinline,minsize,section(".ram_bank128.text")))
void pce_sgx_cache_upload_body(void) {
    uint8_t slot=pce_sgx_sprite_slot;
    pce_sgx_sprite_upload_ok=0;
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2&&
       sprite_slot(pce_sgx_sprite_id,sprite_count[slot])==48)return;
    overlay_call(0x80,pce_sgx_sprite_upload_body);
}
#endif
