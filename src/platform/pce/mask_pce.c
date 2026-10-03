#include "mask_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
static uint32_t mask_address;
static int16_t mask_x,mask_y;
static uint8_t mask_flip,mask_changed,*mask_output;
static uint8_t mask_columns[3][16];
#define MASK_CODE __attribute__((noinline,section(".ram_bank116.text")))
MASK_CODE static void apply(void) {
    const PceScene *s=&pce_scenes[pce_metrics.stage-1];
    int16_t world_x=mask_x+pce_metrics.camera_x;
    int16_t first=world_x>>3;
    mask_changed=0;
    if(mask_y<=-16||mask_y>=224)return;
    uint8_t y0=mask_y<0?0:mask_y;
    uint8_t y1=mask_y+16>224?224:mask_y+16;
    uint8_t bounds[6];bool overlap=false;
    if(first>=0&&(uint16_t)(first+2)<s->cols)arcade_read(1,s->occlusion+(uint16_t)first*2,bounds,6);
    else for(uint8_t k=0;k<3;++k) {
        int16_t col=first+k;bounds[k*2]=255;bounds[k*2+1]=0;
        if(col>=0&&(uint16_t)col<s->cols)arcade_read(1,s->occlusion+(uint16_t)col*2,bounds+k*2,2);
    }
    for(uint8_t k=0;k<3;++k)if(bounds[k*2]<y1&&bounds[k*2+1]>=y0)overlap=true;
    if(!overlap)return;
    for(uint8_t k=0;k<3;++k) {
        int16_t col=first+k;
        for(uint8_t r=0;r<16;++r)mask_columns[k][r]=0;
        if(col>=0&&(uint16_t)col<s->cols)
            arcade_read(1,s->occlusion+(uint32_t)s->cols*2+(uint32_t)col*224+y0,mask_columns[k],y1-y0);
    }
    uint16_t masks[16];
    for(uint8_t y=0;y<16;++y) {
        uint16_t bits=0xffff;
        for(uint8_t x=0;x<16;++x) {
            uint8_t relative=(world_x&7)+x;
            int16_t screen_y=mask_y+y;
            if(screen_y>=y0&&screen_y<y1&&(mask_columns[relative>>3][screen_y-y0]&(0x80>>(relative&7)))) {
                bits&=~(1U<<(mask_flip?x:15-x));mask_changed=1;
            }
        }
        masks[y]=bits;
    }
    if(!mask_changed)return;
    arcade_read(2,mask_address,mask_output,128);
    for(uint8_t plane=0;plane<4;++plane)for(uint8_t y=0;y<16;++y) {
        mask_output[plane*32+y*2]&=masks[y];
        mask_output[plane*32+y*2+1]&=masks[y]>>8;
    }
}
bool mask_pattern(uint32_t address,int16_t x,int16_t y,bool flip,uint8_t *pattern) {
    mask_address=address;mask_x=x;mask_y=y;mask_flip=flip;mask_output=pattern;
    overlay_call(0x74,apply);return mask_changed;
}
