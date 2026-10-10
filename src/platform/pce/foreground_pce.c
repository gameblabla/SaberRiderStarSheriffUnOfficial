#include "presentation_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#include "campaign_pce.h"
#include <string.h>
#ifdef PCE_SGX
#include <pce/bank.h>
#endif
extern uint8_t buffer[2048];
/* Retained foreground. Occluder chunks are static in world space, so each is
 * admitted once, when it enters the window around the camera: it is rendered
 * through the normal sprite cache at a canonical screen position and its SAT
 * entries are kept as world-space parts. Every frame, fg_emit turns the kept
 * parts into SAT entries with one subtraction apiece. Chunk entries are sorted
 * by X, so the window is the index range [lo,hi) found with a cursor. */
#ifdef PCE_SGX
#define FG_MAX 64
#else
#define FG_MAX 37
#endif
#define FG_CODE __attribute__((noinline,section(".ram_bank116.text")))
#ifdef PCE_SGX
#define FG_ENTER_CODE __attribute__((noinline,section(".ram_bank134.text")))
#define FG_ADMIT_CODE __attribute__((noinline,section(".ram_bank114.text")))
#define FG_MAP_CODE __attribute__((noinline,section(".ram_bank116.text")))
#define FG_DRAW_MAP_CODE __attribute__((noinline,section(".ram_bank117.text")))
#else
#define FG_ENTER_CODE __attribute__((noinline,section(".ram_bank116.text")))
#endif
typedef struct {
    uint16_t y_word, x_world, pattern, attribute;
    uint8_t band_first, band_last, slot, spare;
    uint16_t index;
} FgPart;
#ifdef PCE_SGX
/* The full SGX foreground window needs up to 59 retained sprite pieces. The
 * PCE work bank is full, so keep its persistent list in CD-RAM bank 134. */
FgPart fg_parts[FG_MAX] __attribute__((section(".ram_bank134.bss")));
static uint32_t sgx_foreground_base PCE_WORK;
static uint16_t sgx_foreground_count PCE_WORK;
static uint8_t sgx_fg_vdc0_count PCE_WORK,sgx_fg_vdc0_last PCE_WORK;
static uint16_t enter_sprite_id PCE_WORK;
static int16_t enter_sprite_y PCE_WORK;
static bool enter_sprite_admitted PCE_WORK;
#else
FgPart fg_parts[FG_MAX] PCE_WORK;
#endif
uint8_t fg_count PCE_WORK,fg_slow_count PCE_WORK,fg_entered;
uint16_t fg_camera PCE_WORK,fg_camera_slow PCE_WORK;
static uint16_t window_lo[2] PCE_WORK,window_hi[2] PCE_WORK,last_camera[2] PCE_WORK;
static bool window_valid[2] PCE_WORK;
static uint32_t slow_base PCE_WORK;
static uint16_t slow_count PCE_WORK;
#ifdef PCE_SGX
static const PceScene *slow_scene PCE_WORK;
static bool slow_loaded PCE_WORK;
static uint16_t next_window_camera[2] PCE_WORK;
#endif
static uint32_t enter_base PCE_WORK;
static uint16_t enter_index PCE_WORK;
static uint8_t enter_group PCE_WORK;
extern uint8_t sat_count,sat_page,sprite_screen_height,sprite_exact,sprite_last_free;
extern vdc_sprite_t sat[2][64];
extern uint8_t sprite_occupancy[240];
extern void sprite_lines_clear(void);
extern void fg_emit(void);

void foreground_prepare_body(void);
#ifdef PCE_SGX
__attribute__((noinline,section(".ram_bank107.text")))
#endif
void foreground_reset(void) {
    memset(window_valid,0,sizeof window_valid);fg_count=fg_slow_count=0;
#ifdef PCE_SGX
    slow_scene=0;slow_loaded=false;sgx_foreground_base=0;sgx_foreground_count=0;
#endif
}
FG_ENTER_CODE static void fg_read(uint32_t base,uint16_t index,void *out,uint16_t bytes) {
    uint16_t offset=(index<<2)+(index<<1);
    arcade_read(2,base+offset,out,bytes);
}
FG_ENTER_CODE static int16_t chunk_x(uint32_t base,uint16_t index) {
    int16_t x;fg_read(base,index,&x,2);return x;
}
FG_ENTER_CODE static uint16_t locate(uint32_t base,uint16_t n,uint16_t from,int16_t key,bool far) {
    if(far) {
        uint16_t lo=0,hi=n;
        while(lo<hi) {
            uint16_t mid=lo+(hi-lo)/2;
            if(chunk_x(base,mid)<key)lo=mid+1;else hi=mid;
        }
        return lo;
    }
    while(from>0&&chunk_x(base,from-1)>=key)--from;
    while(from<n&&chunk_x(base,from)<key)++from;
    return from;
}
/* Admit one chunk, then move its SAT entries into the retained list. */
#ifdef PCE_SGX
FG_ADMIT_CODE static void enter_admit_body(void) {
    uint8_t bank=pce_bank6_get();
    pce_bank6_set(108);  /* video_sprite's descriptor and upload scratch live in PCE_STAGE */
    enter_sprite_admitted=video_sprite_optional(enter_sprite_id,96,enter_sprite_y,false,16);
    pce_bank6_set(bank);
}
#endif
FG_ENTER_CODE static void enter_body(void) {
    int16_t entry[3];
    uint16_t offset=(enter_index<<2)+(enter_index<<1);
    arcade_read(2,enter_base+offset,entry,6);
#ifdef PCE_SGX
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2) {
        /* These are temporary canonical-X entries, not a displayed SAT.
         * Chunks at different world positions must not compete at X=96
         * while building the retained list. Admission runs again at draw. */
        sat_count=0;sprite_lines_clear();
    }
#endif
    uint8_t before=sat_count;
#ifdef PCE_SGX
    enter_sprite_id=entry[2];enter_sprite_y=entry[1];
    overlay_call(0x72,enter_admit_body);
    bool admitted=enter_sprite_admitted;
#else
    bool admitted=video_sprite_optional(entry[2],96,entry[1],false,16);
#endif
    if(!admitted)return;
    uint8_t slot=sprite_slot_of[entry[2]];
    vdc_sprite_t *source=sat[sat_page];
    for(uint8_t k=before;k<sat_count&&fg_count<FG_MAX;++k) {
        uint8_t at=enter_group?fg_count:fg_slow_count;
        if(at<fg_count)fg_parts[fg_count]=fg_parts[at];
        FgPart *p=&fg_parts[at];
        int16_t sy=(int16_t)source[k].y-64;
        uint8_t lo=sy<0?0:sy,hi=sy+16>sprite_screen_height?sprite_screen_height:sy+16;
        p->y_word=source[k].y;
        p->x_world=source[k].x+(entry[0]-96);
        p->pattern=source[k].pattern;p->attribute=source[k].attr;
        p->band_first=sprite_exact?lo:lo>>3;p->band_last=sprite_exact?hi-1:(hi-1)>>3;p->slot=slot;p->spare=enter_group;p->index=enter_index;
        ++fg_count;if(!enter_group)++fg_slow_count;
    }
}
FG_CODE static void enter(uint32_t base,uint16_t index,uint8_t group) {
    enter_base=base;enter_index=index;enter_group=group;
    enter_body();
}
FG_CODE static void drop(uint8_t group,uint16_t lo,uint16_t hi) {
    uint8_t k=0;
    while(k<fg_count) {
        FgPart *p=&fg_parts[k];
        if((p->spare&1)==group&&p->index>=lo&&p->index<hi) {
            if(!group)--fg_slow_count;
            memmove(p,p+1,(uint16_t)(fg_count-k-1)*sizeof(FgPart));--fg_count;
        } else ++k;
    }
}
/* Called right after video_sat_begin: leaves SAT and band counts empty. */
FG_CODE void foreground_prepare_body(void) {
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    uint16_t camera=pce_metrics.camera_x;
#ifdef PCE_SGX
    bool split_foreground=pce_sgx_gameplay()&&
        (pce_metrics.stage==1||pce_metrics.stage==3||pce_metrics.stage==4||pce_metrics.stage==5);
    if(slow_scene!=s) {slow_scene=s;slow_base=0;slow_count=0;slow_loaded=false;sgx_foreground_base=0;sgx_foreground_count=0;}
    if(!slow_loaded&&pce_sgx_gameplay()&&pce_metrics.stage<=5&&pce_metrics.stage!=2&&s->occlusion) {
        PceSgxSkyRecord record={0};
        if(arcade_read(2,s->occlusion,&record,sizeof record)) {
            sgx_foreground_base=record.foreground;sgx_foreground_count=record.nforeground;
            slow_base=record.foreground_slow;slow_count=record.nforeground_slow;
            if(record.nforeground||record.nforeground_slow) {
                sprite_cache_foreground_first=record.foreground_first;
                sprite_cache_stage=pce_metrics.stage;
            }
        }
        slow_loaded=true;
    }
    uint8_t saved_page=sat_page;
    if(split_foreground) {
        sgx_fg_vdc0_count=sat_count;sgx_fg_vdc0_last=sprite_last_free;
        sat_count=0;sprite_lines_clear();
    }
#else
    slow_base=0;slow_count=0;
#endif
    fg_camera=camera;fg_camera_slow=camera+camera/5;
    uint16_t normal_count=s->nforeground;
#ifdef PCE_SGX
    if(!pce_sgx_gameplay()&&(pce_metrics.stage==1||pce_metrics.stage==3))normal_count=0;
    uint32_t normal_base=s->foreground;
    if(split_foreground) {normal_base=sgx_foreground_base;normal_count=sgx_foreground_count;}
#else
    uint32_t normal_base=s->foreground;
#endif
    uint8_t entered=0;
    for(uint8_t group=0;group<2;++group) {
        uint32_t base=group?normal_base:slow_base;
        uint16_t n=group?normal_count:slow_count;
        if(!n) {
#ifdef PCE_SGX
            next_window_camera[group]=65535;
#endif
            continue;
        }
        uint16_t layer_camera=group?camera:fg_camera_slow;
        int16_t start=(int16_t)layer_camera-31,end=(int16_t)layer_camera+256;
        int16_t moved=(int16_t)(layer_camera-last_camera[group]);
        bool far=!window_valid[group]||moved>48||moved<-48;
        last_camera[group]=layer_camera;
        uint16_t need_lo=locate(base,n,window_lo[group],start,far);
        uint16_t need_hi=locate(base,n,window_hi[group]<need_lo?need_lo:window_hi[group],end,far);
        if(far) {drop(group,0,n);window_lo[group]=window_hi[group]=need_lo;window_valid[group]=true;}
        while(need_lo<window_lo[group]) {--window_lo[group];enter(base,window_lo[group],group);entered=1;}
        while(window_hi[group]<need_hi) {enter(base,window_hi[group],group);++window_hi[group];entered=1;}
        if(window_lo[group]<need_lo) {drop(group,window_lo[group],need_lo);window_lo[group]=need_lo;}
        if(window_hi[group]>need_hi) {drop(group,need_hi,window_hi[group]);window_hi[group]=need_hi;}
#ifdef PCE_SGX
        uint16_t next=65535;
        if(need_lo<need_hi)next=(uint16_t)(chunk_x(base,need_lo)+32);
        if(need_hi<n) {
            uint16_t enter_at=(uint16_t)(chunk_x(base,need_hi)-255);
            if(enter_at<next)next=enter_at;
        }
        next_window_camera[group]=next;
#endif
    }
    fg_entered=entered;
#ifdef PCE_SGX
    if(split_foreground) {
        /* Admit and retain on VDC0, then restore the HUD/hero prefix.
           All foreground pieces are emitted once, ahead of the actors. */
        sat_page=saved_page;sat_count=sgx_fg_vdc0_count;sprite_last_free=sgx_fg_vdc0_last;
    } else
#endif
    if(entered) {sat_count=0;memset(sprite_occupancy,0,sprite_exact?240:32);}
}
#ifdef PCE_SGX
FG_MAP_CODE void foreground_prepare(void) {
    uint16_t camera=pce_metrics.camera_x;
    uint16_t slow=camera+camera/5;
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2&&
       slow_loaded&&slow_scene==&pce_scenes[pce_metrics.stage-1]&&camera>=fg_camera&&
       (camera==fg_camera || (camera<next_window_camera[1]&&slow<next_window_camera[0]))) {
        /* The retained window is unchanged between chunk boundaries. Move
         * its coordinates without clearing or copying either SAT budget. */
        fg_camera=camera;fg_camera_slow=slow;fg_entered=0;return;
    }
    /* Stage scratch is accessible only while MPR6 holds bank 108. */
    pce_sgx_copy(buffer+1024,sat[0],(uint16_t)sat_count*8);
    pce_sgx_copy(buffer+1536,sprite_occupancy,240);
    uint8_t bank=pce_bank6_get();pce_bank6_set(134);
    overlay_call(0x74,foreground_prepare_body);
    pce_bank6_set(bank);
    if(pce_sgx_gameplay()&&pce_metrics.stage<6&&pce_metrics.stage!=2) {
        pce_sgx_copy(sat[0],buffer+1024,(uint16_t)sat_count*8);
        pce_sgx_copy(sprite_occupancy,buffer+1536,240);
    }

}
#else
void foreground_prepare(void) { overlay_call(0x74,foreground_prepare_body); }
#endif
#ifdef PCE_SGX
FG_DRAW_MAP_CODE void foreground_draw(void) {
    video_front_begin();
    uint8_t bank=pce_bank6_get();pce_bank6_set(134);
    overlay_call(0x74,fg_emit);
    pce_bank6_set(bank);
}
#else
void foreground_draw(void) {
    video_front_begin();buffer[799]=0;overlay_call(0x74,fg_emit);
}
#endif
