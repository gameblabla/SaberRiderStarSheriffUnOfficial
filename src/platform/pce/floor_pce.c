#include "floor_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "assets.h"
#include "overlay_pce.h"
#include <string.h>

/* The race floor: 24 BAT rows of 64 pair characters (128 samples across the 512-dot line), each row its own perspective row
 * (4 scanlines), for any of 128 headings (the projection table is in the race archive). A new floor is sampled a few rows at a
 * time between frames into the page not on display, and shown once complete (floor_present at the next VBlank). */
uint8_t floor_pairs[64];
extern volatile uint16_t floor_u,floor_v,floor_du,floor_dv;
extern volatile uint8_t floor_road_hi,floor_road_b2;
void floor_sample_map(void),floor_sample_half(void),floor_sample_road(void);
_Static_assert(PCE_RACE_RACE_MAP==0,"Sampler needs the map at Arcade offset zero");

static uint8_t floor_row,back_page,snapshot_heading,snapshot_phase;
static uint16_t snapshot_x,snapshot_y;
static uint8_t ready_page;
uint16_t floor_shown_x,floor_shown_y;uint8_t floor_shown_heading;static uint8_t shown_valid;
extern volatile uint8_t pce_floor_pending;
/* Samples per call adapt to the loop: it keeps to two frames a pass or a little over (a third frame is the signal to back off, since the
 * wait for the next vertical blank would waste what a short pass leaves). */
static uint16_t budget=384;

PCE_FLOOR bool floor_init(void) {
    arcade_seek(1,0);
    floor_row=0;shown_valid=0;pce_floor_pending=pce_floor_page=0;
    video_display(false); video_race_init(); video_display(false);
    if(!video_race_sky()) return false;
    for(uint8_t row=0;row<24;++row) {
        for(uint8_t k=0;k<64;++k) floor_pairs[k]=((k+row)&1)?0x23:0x32;
        video_floor_row(0,row,floor_pairs);video_floor_row(1,row,floor_pairs);
    }
    video_display(true);
    return true;
}
PCE_FLOOR void floor_update(uint16_t x,uint16_t y,uint8_t heading,uint8_t phase) {
    if(!floor_row) {
        snapshot_x=x;snapshot_y=y;snapshot_heading=heading&127;snapshot_phase=phase;
        back_page=pce_floor_page^1;
        if(!shown_valid){shown_valid=1;floor_shown_x=x;floor_shown_y=y;floor_shown_heading=snapshot_heading;}
    }
    /* A call samples about `budget` pairs (a near row is 128, a far one 64): the far half of the floor (rows 0-11) takes one sample per pair, the near half two. */
    if(!floor_row) {}
    else if(pce_control.elapsed<=3){if(budget<2304)budget+=32;}
    else if(budget>256)budget-=128;
    for(uint16_t used=0;floor_row<24;) {
        int16_t g[4];
        uint8_t row=floor_row;
        bool far=row<12&&!snapshot_phase;
        if(used&&used+(far?64:128)>budget)break;
        arcade_read(2,PCE_RACE_FLOOR_GEOMETRY+((uint32_t)snapshot_heading*24+row)*8,g,8);
        floor_u=(snapshot_x<<3)+g[0];floor_v=(snapshot_y<<3)+g[1];
        floor_du=far?g[2]*2:g[2];floor_dv=far?g[3]*2:g[3];
        if(snapshot_phase){floor_road_hi=(PCE_RACE_PURSUIT_ROW>>8)&255;floor_road_b2=PCE_RACE_PURSUIT_ROW>>16;floor_sample_road();}
        else if(far)floor_sample_half();
        else floor_sample_map();
        video_floor_row(back_page,row,floor_pairs);
        ++floor_row;used+=far?64:128;
    }
    if(floor_row==24) {
        ready_page=back_page;floor_row=0;++pce_metrics.floor_commits;
        floor_shown_x=snapshot_x;floor_shown_y=snapshot_y;floor_shown_heading=snapshot_heading;
    }
}
PCE_FLOOR void floor_present(void) { pce_floor_pending=ready_page; }
