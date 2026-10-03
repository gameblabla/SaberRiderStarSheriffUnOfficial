#include "floor_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include <string.h>
#include "floor_geometry.h"

static uint8_t mip[704] PCE_STAGE;
uint8_t floor_mip_lo[11], floor_mip_hi[11];
uint8_t floor_pairs[64];
extern volatile uint16_t floor_u,floor_v,floor_du,floor_dv;
extern volatile uint8_t floor_phase;
void floor_sample(void);
_Static_assert(PCE_RACE_RACE_MAP==0,"Sampler needs the map at Arcade offset zero");

static uint8_t floor_row, back_page, snapshot_heading, snapshot_phase;
static uint16_t snapshot_x, snapshot_y;
static uint8_t ready_page;
extern volatile uint8_t pce_floor_pending;

PCE_FLOOR bool floor_init(void) {

    arcade_read(1,PCE_RACE_FLOOR_MIPS,mip,sizeof mip);
    for(uint8_t i=0;i<11;++i){uint16_t a=(uint16_t)(mip+i*64);floor_mip_lo[i]=a;floor_mip_hi[i]=a>>8;}
    arcade_seek(1,0);
    floor_row=0;pce_floor_pending=pce_floor_page=0;
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
        snapshot_x=x;snapshot_y=y;snapshot_heading=heading&31;snapshot_phase=phase;
        back_page=pce_floor_page^1;
    }
    /* Eight depth samples, each repeated over three BAT rows, preserve
     * the 96-line floor window within the measured CPU budget. */
    for(uint8_t job=0;job<8;++job) {
        uint8_t row=floor_row++*3;
        const int16_t *g=geometry[snapshot_heading][row+1];
        floor_u=(snapshot_x<<3)+g[0];floor_v=(snapshot_y<<3)+g[1];
        floor_du=g[2];floor_dv=g[3];floor_phase=snapshot_phase;floor_sample();
        for(uint8_t repeat=0;repeat<3;++repeat)video_floor_row(back_page,row+repeat,floor_pairs);
        if(floor_row==8) {
            ready_page=back_page;floor_row=0;++pce_metrics.floor_commits;
            break;
        }
    }
}
PCE_FLOOR void floor_present(void) { pce_floor_pending=ready_page; }
