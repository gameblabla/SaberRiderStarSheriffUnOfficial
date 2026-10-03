#include "presentation_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
#include "overlay_pce.h"
#include <string.h>
/* Retained foreground. Occluder chunks are static in world space, so each is
 * admitted once, when it enters the window around the camera: it is rendered
 * through the normal sprite cache at a canonical screen position and its SAT
 * entries are kept as world-space parts. Every frame, fg_emit turns the kept
 * parts into SAT entries with one subtraction apiece. Chunk entries are sorted
 * by X, so the window is the index range [lo,hi) found with a cursor. */
#define FG_MAX 40
#define FG_CODE __attribute__((noinline,section(".ram_bank116.text")))
typedef struct {
    uint16_t y_word, x_world, pattern, attribute;
    uint8_t band_first, band_last, slot, spare;
    uint16_t index;
} FgPart;
FgPart fg_parts[FG_MAX] PCE_WORK;
uint8_t fg_count;
uint16_t fg_camera;
static uint16_t window_lo, window_hi, last_camera;
static bool window_valid;
extern uint8_t sat_count,sprite_screen_height;
extern vdc_sprite_t sat[2][64];
extern uint8_t sprite_occupancy[240];
extern void fg_emit(void);

void foreground_prepare_body(void);
void foreground_reset(void) { window_valid=false;fg_count=0; }
FG_CODE static void fg_read(const PceScene *s,uint16_t index,void *out,uint16_t bytes) {
    uint16_t offset=(index<<2)+(index<<1);
    arcade_read(2,s->foreground+offset,out,bytes);
}
FG_CODE static int16_t chunk_x(const PceScene *s,uint16_t index) {
    int16_t x;fg_read(s,index,&x,2);return x;
}
FG_CODE static uint16_t locate(const PceScene *s,uint16_t from,int16_t key,bool far) {
    uint16_t n=s->nforeground;
    if(far) {
        uint16_t lo=0,hi=n;
        while(lo<hi) {
            uint16_t mid=lo+(hi-lo)/2;
            if(chunk_x(s,mid)<key)lo=mid+1;else hi=mid;
        }
        return lo;
    }
    while(from>0&&chunk_x(s,from-1)>=key)--from;
    while(from<n&&chunk_x(s,from)<key)++from;
    return from;
}
/* Admit one chunk, then move its SAT entries into the retained list. */
FG_CODE static void enter(const PceScene *s,uint16_t index) {
    int16_t entry[3];
    fg_read(s,index,entry,6);
    uint8_t before=sat_count;
    if(!video_sprite_optional(entry[2],96,entry[1],false,16))return;
    uint8_t slot=sprite_slot_of[entry[2]];
    for(uint8_t k=before;k<sat_count&&fg_count<FG_MAX;++k) {
        FgPart *p=&fg_parts[fg_count++];
        int16_t sy=(int16_t)sat[0][k].y-64;
        uint8_t lo=sy<0?0:sy,hi=sy+16>sprite_screen_height?sprite_screen_height:sy+16;
        p->y_word=sat[0][k].y;
        p->x_world=sat[0][k].x+(entry[0]-96);
        p->pattern=sat[0][k].pattern;p->attribute=sat[0][k].attr;
        p->band_first=lo>>3;p->band_last=(hi-1)>>3;p->slot=slot;p->index=index;
    }
}
static void drop(uint16_t lo,uint16_t hi) {
    uint8_t k=0;
    while(k<fg_count) {
        if(fg_parts[k].index>=lo&&fg_parts[k].index<hi)fg_parts[k]=fg_parts[--fg_count];
        else ++k;
    }
}
/* Called right after video_sat_begin: leaves SAT and band counts empty. */
FG_CODE void foreground_prepare_body(void) {
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    uint16_t camera=pce_metrics.camera_x;
    int16_t start=(int16_t)camera-31,end=(int16_t)camera+256;
    int16_t moved=(int16_t)(camera-last_camera);
    bool far=!window_valid||moved>48||moved<-48;
    last_camera=camera;fg_camera=camera;
    uint16_t need_lo=locate(s,window_lo,start,far),need_hi=locate(s,window_hi<need_lo?need_lo:window_hi,end,far);
    if(far) { fg_count=0;window_lo=window_hi=need_lo;window_valid=true; }
    uint8_t entered=0;
    while(need_lo<window_lo) {--window_lo;enter(s,window_lo);entered=1;}
    while(window_hi<need_hi) {enter(s,window_hi);++window_hi;entered=1;}
    if(window_lo<need_lo) {drop(window_lo,need_lo);window_lo=need_lo;}
    if(window_hi>need_hi) {drop(need_hi,window_hi);window_hi=need_hi;}
    if(entered) {sat_count=0;memset(sprite_occupancy,0,32);}
}
void foreground_prepare(void) { overlay_call(0x74,foreground_prepare_body); }
void foreground_draw(void) { video_front_begin();fg_emit(); }
