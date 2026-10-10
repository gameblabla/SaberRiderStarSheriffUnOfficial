#ifdef PCE_SGX
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "assets.h"
#include "race_pce.h"
#define RACE_SIN_SECTION ".ram_bank128.rodata"
#include "race_math.h"
/* The fixed track's perspective is baked at 1/16 of a segment. Only the
 * camera's translation and small yaw lag remain dynamic. The old knot builder
 * stays callable through road_reference for native comparison fixtures. */
#define BAKED __attribute__((noinline,section(".ram_bank128.text")))
#define CURVE_A ((int16_t *)((uint8_t *)columns+1664))
#define CURVE_B ((int16_t *)((uint8_t *)columns+1792))
uint8_t road_baked_frac;
int32_t road_baked_bias;
int16_t road_baked_step;
_Static_assert(sizeof columns>=1894,"Road curve staging overlaps the work bank");
void road_baked_fill(void);
BAKED void road_baked_call(void) {
    int16_t side,yaw=0;
    road_baked_frac=ps&15;
    if(rphase<P_PURSUIT) {
        uint16_t index=ps>>4,next=(index+1)&4095;
        arcade_read(1,PCE_RACE_ROAD_CURVES+((uint32_t)index<<7),CURVE_A,102);
        arcade_read(1,PCE_RACE_ROAD_CURVES+((uint32_t)next<<7),CURVE_B,102);
        int16_t cx=CURVE_A[48]+((wrapdiff(CURVE_B[48],CURVE_A[48])*road_baked_frac)>>4);
        int16_t cy=CURVE_A[49]+((wrapdiff(CURVE_B[49],CURVE_A[49])*road_baked_frac)>>4);
        uint16_t heading=CURVE_A[50]+(((int16_t)(CURVE_B[50]-CURVE_A[50])*road_baked_frac)>>4);
        int16_t rx=wrapdiff(cx,pce_control.x),ry=wrapdiff(cy,pce_control.y);
        /* Translation measured in the actual camera's frame. */
        side=(int16_t)(((race_mulw(ry,cam_c)-race_mulw(rx,cam_s))*128
            +race_mulw(ry,cam_cl)-race_mulw(rx,cam_sl))>>12);
        yaw=(int16_t)(cam_hd-heading);
    } else {
        side=(int16_t)(race_mulw((int16_t)(4096-pce_control.x),127)>>5);
    }
    /* x = side*d*77/512 in Q4 dots; yaw's focal factor is
       10080*0.0376*16*2pi/65536 = 0.5814 Q4 dots/angle unit.
       BXR uses Q8 dots and the first raster index is m=16, d=15. */
    int32_t lateral=race_mulw(side,77);
    road_baked_step=-(lateral>>4); /* two scanlines per raster pair */
    road_baked_bias=65536L+((race_mulw(yaw,-107)+((int32_t)yaw*256))>>4)
        -((lateral*16-lateral)>>5); /* yaw * 149 without a long multiply */
    road_baked_fill();
}
#endif
