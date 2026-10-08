#include "play_internal.h"
#include "sprite_cache_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "audio_pcm.h"
#include "sgx_pce.h"
/* The robot-horse herd (level 1's stampede) at the source's full size. A frame is 128x80, drawn as VDC big sprite
 * cells: four columns of a 32x64 and a 32x16 sprite (8 SAT entries a horse, not 33 pieces). The frame patterns are
 * streamed from the scene into pages of the sprite cache that the herd reserves for its length, into one of two
 * buffers so the frame on screen is never overwritten while it is displayed. */
#define HERD_CODE __attribute__((noinline,section(".ram_bank111.text")))
extern uint8_t sat_count,sat_page;
extern vdc_sprite_t sat[2][64];
extern uint8_t sprite_line_lo,sprite_line_hi,sprite_line_ok;
extern void sprite_lines_reserve(void),sprite_lines_release(void),sprite_lines_wide(void);
extern uint8_t sprite_line_cells;
extern uint8_t sprite_exact;
typedef struct {uint16_t x,y,top,bottom;uint8_t ntop,nbottom,cells;} HerdEmit;
_Static_assert(sizeof(HerdEmit)==11,"herd_emit.S parameter layout changed");
HerdEmit herd_emit_args;
extern void herd_emit(void),herd_prepare(void);
typedef struct {int16_t x,y;uint8_t first,last;} HerdHorse;
_Static_assert(sizeof(HerdHorse)==6,"herd_prepare.S record layout changed");
HerdHorse herd_horses[8];
uint8_t herd_live,herd_horse_count,herd_cells,herd_same;
uint8_t herd_on,herd_locked;
int16_t herd_y;
uint16_t herd_lead;
static uint8_t shown,cur,prefetch;
static uint16_t prefetched;
uint16_t warm_ids[7];
uint8_t warm_slots[7],warm_count;
#define BUFFER_PAGE(b) (28+(b)*10)
#ifdef PCE_SGX
static uint16_t herd_sgx_code;
/* Reserve all five poses for the entire scene, before any actor SAT can
   reference their pages. Starting a herd never overwrites displayed actors. */
__attribute__((noinline,minsize,section(".ram_bank128.text")))
void pce_sgx_herd_sky_load_body(void) {
    uint32_t horse=pce_scenes[pce_metrics.stage-1].horse;
    if(!horse)return;
    for(uint8_t k=0;k<5;++k) {
        uint16_t word=k<3?0x2800+(uint16_t)k*0xa00:
            PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(k-3)*256;
        arcade_vram_to(1,horse+32+(uint32_t)k*5120,word,5120);
    }
}
__attribute__((noinline,section(".ram_bank128.text")))
#else
HERD_CODE
#endif
void herd_reserve(void) {
    uint8_t colors[32];
    for(uint8_t p=28;p<48;++p) {
#ifdef PCE_SGX
        if(pce_sgx_gameplay())continue;
#endif
        uint8_t owner=pattern_owner[p];
        if(owner&&owner!=48) {
            sprite_ids[owner-1]=0xffff;
            for(uint8_t q=0;q<54;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
        }
        pattern_owner[p]=48;
    }
    if(!pce_sgx_gameplay())sprite_pinned[47]=250;
    herd_on=1;shown=0xff;cur=0;
    prefetch=0xff;prefetched=0;warm_count=0;
    arcade_read(2,play_scene->horse,colors,32);
    pce_vce_copy_palette(pce_sgx_gameplay()?29:31,colors,1);
}
/* Starts the convoy: the first horses are created by herd_feed (encounter_pce.c), the rest as the column nears. */
HERD_CODE void herd_spawn(void) {
#ifdef PCE_SGX
    overlay_call(0x80,herd_reserve);
#else
    herd_reserve();
#endif
}
/* Streaming runs in the flow overlay; the renderer and IRQs remain mapped. */
#ifdef PCE_SGX
extern volatile uint8_t pce_sat_pending;
__attribute__((noinline,minsize,section(".ram_bank128.text")))
static void herd_stream_sgx(void) {
    uint8_t pose=(frame>>2)%5;
    uint16_t word=pose<3?0x2800+(uint16_t)pose*0xa00:
        PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(pose-3)*256;
    herd_sgx_code=word>>5;
}
#endif
PCE_FLOW static void herd_stream(void) {
#ifdef PCE_SGX
    if(pce_sgx_gameplay()){overlay_call(0x80,herd_stream_sgx);return;}
#endif
    uint8_t target_vdc=0;
#ifdef PCE_SGX
    if(pce_sgx_gameplay())target_vdc=1;
#endif
    uint8_t want=(frame>>2)%5;
    if(want!=shown) {
        /* Finish a partially prefetched frame after a missed simulation tick.
         * Only publish a complete alternate buffer. */
        if(prefetch!=want)prefetched=0;
        cur^=1;
        if(prefetched<5120) {
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            arcade_vram_to(target_vdc,play_scene->horse+32+(uint32_t)want*5120+prefetched,
                PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(cur)*256+(prefetched>>1),5120-prefetched);
        }
        shown=want;prefetch=(want+1)%5;prefetched=0;
    }
    /* Start reusing the previous buffer after its final display VBlank.
     * Four equal 1280-byte slices bound the work of every draw; the last
     * slice finishes above, just before publishing the new animation.
     * Three larger slices could miss a VBlank when firing began, then the
     * skipped simulation phase forced another oversized catch-up upload. */
    uint8_t phase=frame&3;
    uint16_t target=phase==1?1280:phase==2?2560:phase==3?3840:0;
    if(target>prefetched) {
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram_to(target_vdc,play_scene->horse+32+(uint32_t)prefetch*5120+prefetched,
            PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(cur^1)*256+(prefetched>>1),target-prefetched);
        prefetched=target;
    }
}
/* Use the offscreen approach to retain the two firing poses, four straight
 * muzzle frames and projectile. Their nine pattern pages fit beside the HUD
 * and both displayed idle generations, with the herd's buffers reserved. */
HERD_CODE static void herd_warm(void) {
#ifdef PCE_SGX
    /* The hero needs all three pose buffers for displayed generations.
       Warming two additional poses would prevent the next idle/run pose
       from being allocated. Warm only flashes and the projectile on SGX. */
    uint8_t total=5,offset=2;
#else
    uint8_t total=7,offset=0;
#endif
    if(warm_count<total) {
        uint8_t i=warm_count+offset,stage=pce_metrics.stage-1;
        uint16_t id=i<2?pce_motion_base[stage]+hero*3+i:
            i<6?pce_flash_base[stage]+4+i-2:36;
        if(video_sprite_optional(id,-256,240,false,16)) {
            uint8_t slot=sprite_slot_of[id];
            if(slot<48&&sprite_ids[slot]==id){warm_ids[warm_count]=id;warm_slots[warm_count]=slot;++warm_count;}
        }
    }
    for(uint8_t i=0;i<warm_count;++i)if(sprite_ids[warm_slots[i]]==warm_ids[i])sprite_pinned[warm_slots[i]]=250;
}
/* Draw every horse; releases the pages once the last one has gone. */
HERD_CODE void herd_draw(void) {
    overlay_call(0x6e,herd_prepare);
    if(!herd_live&&!herd_pending) {
        if(!pce_sgx_gameplay())for(uint8_t p=28;p<48;++p)pattern_owner[p]=0;
        for(uint8_t i=0;i<warm_count;++i)if(sprite_ids[warm_slots[i]]==warm_ids[i])
            sprite_pinned[warm_slots[i]]=sprite_used[warm_slots[i]]?2:0;
        if(!pce_sgx_gameplay())sprite_pinned[47]=0;
        herd_on=herd_locked=herd_flee=0;audio_pcm_gallop(false);return;
    }
    audio_pcm_gallop(true);
    if(!pce_sgx_gameplay())sprite_pinned[47]=250;
    overlay_call(0x6e,herd_stream);
    herd_warm();
    uint16_t code=(PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(cur)*256)>>5;
#ifdef PCE_SGX
    if(pce_sgx_gameplay())code=herd_sgx_code;
#endif
    bool shared=herd_horse_count&&herd_same&&sat_count+2*herd_cells<=64&&
        (sprite_exact||!(herd_horses[0].y&7));
    uint8_t remaining[2]={0,0};
    for(uint8_t k=0;k<herd_horse_count;++k) {
        const HerdHorse *horse=&herd_horses[k];
        int16_t sx=horse->x,sy=horse->y;
        uint8_t first=horse->first,last=horse->last;
        uint8_t cells=last-first,accepted[2]={0,0};
        /* Batch disjoint spans with enough SAT slots. Unaligned band-mode
         * spans share an edge band, so preserve interleaved admission there
         * and near a full SAT. The first columns still win. */
        bool batch=sat_count+2*cells<=64&&(sprite_exact||!(sy&7));
        if(batch)for(uint8_t part=0;part<2;++part) {
            int16_t y=sy+(part?64:0),h=part?16:64;
            int16_t lo=y<0?0:y,hi=y+h>224?224:y+h;
            if(hi<=lo||!cells)continue;
            uint8_t available;
            if(shared&&k)available=remaining[part];
            else {
                sprite_line_lo=lo;sprite_line_hi=hi;sprite_line_cells=shared?herd_cells:cells;
                overlay_call(0x6e,sprite_lines_wide);available=sprite_line_cells;
            }
            accepted[part]=available<cells?available:cells;
            if(shared)remaining[part]=available-accepted[part];
        }
        if(batch) {
            herd_emit_args=(HerdEmit){sx+32*first+32,sy+64,code+16*first,
                code+64+4*first,accepted[0],accepted[1],cells};
            if(cells)herd_emit();
            continue;
        }
        for(uint8_t c=first;c<last;++c) {
            int16_t x=sx+32*c;
            for(uint8_t part=0;part<2;++part) {
                int16_t y=part?sy+64:sy,h=part?16:64,lo=y<0?0:y,hi=y+h>224?224:y+h;
                if(sat_count>=64||hi<=lo)continue;
                sprite_line_lo=lo;sprite_line_hi=hi;
                sprite_lines_reserve();
                if(!sprite_line_ok)continue;
                sprite_lines_reserve();
                if(!sprite_line_ok){sprite_lines_release();continue;}
                sat[sat_page][sat_count++]=(vdc_sprite_t){y+64,x+32,code+(part?32+2*c:8*c)*2,
                    VDC_SPRITE_FG|(pce_sgx_gameplay()?13:15)|VDC_SPRITE_WIDTH_32|(part?0:VDC_SPRITE_HEIGHT_64)};
            }
        }
    }
}
