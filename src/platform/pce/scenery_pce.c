#include "scenery_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "race_pce.h"
#include "floor_pce.h"
#include "campaign_pce.h"

extern uint8_t buffer[2048];
uint8_t space_hull_ready,space_hull_top[24],space_hull_bottom[24];
static uint16_t flash_palette[512] PCE_STAGE;
static uint16_t hull_palette[64] PCE_STAGE;
uint8_t space_flashing;
static uint8_t hull_gone;
volatile uint16_t pce_sky_far,pce_sky_near;

PCE_SCENERY void race_scenery(void) {
    /* Horizon follows the completed road camera; high clouds move half as far.
     * The 512-dot texture repeats twice in the 1024-dot BAT. */
    pce_sky_far=((uint16_t)floor_shown_heading*4)&511;
    pce_sky_near=((uint16_t)floor_shown_heading*8+(floor_shown_y>>4))&511;
}

PCE_SCENERY void space_hull_load(void) {
    uint32_t a=pce_boss_bg[6];
    uint16_t count;
    arcade_read(2,a,&count,2);
    arcade_read(2,a+4,space_hull_top,24);arcade_read(2,a+28,space_hull_bottom,24);
    /* Fade just the nebula. Sprites and music continue; every asset is already
     * in Arcade RAM, so the transition performs no disc reads. */
    pce_vce_copy_palette_to_ram(flash_palette,0,16);
    for(uint8_t level=1;level<=7;++level) {
        for(uint16_t i=0;i<256;++i) {
            uint16_t c=flash_palette[i];uint8_t b=c&7,r=(c>>3)&7,g=(c>>6)&7;
            b=b>level?b-level:0;r=r>level?r-level:0;g=g>level?g-level:0;
            ((uint16_t*)buffer)[i]=((uint16_t)g<<6)|((uint16_t)r<<3)|b;
        }
        video_wait();pce_vce_copy_palette(0,buffer,16);video_wait();
    }
    arcade_read(2,a+64,hull_palette,128);
    video_display(false);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(a+832,PCE_BG_WORD,count*32);
    video_vdc(0,0);
    for(uint16_t i=0;i<2048;++i)video_vdc(2,PCE_BG_WORD>>4);
    arcade_read(2,a+192,buffer,23*13*2);
    for(uint8_t y=0;y<13;++y) {
        video_vdc(0,(uint16_t)y*64);
        for(uint8_t x=0;x<23;++x)video_vdc(2,((uint16_t*)buffer)[(uint16_t)y*23+x]+(PCE_BG_WORD>>4));
    }
    pce_vce_copy_palette(0,hull_palette,4);
    pce_vce_set_color(255,0x1ff);   /* the ending dialogue still needs white BG glyphs */
    video_scroll((uint16_t)-76,(uint16_t)-60);
    space_flashing=hull_gone=0;space_hull_ready=1;
    video_wait();video_display(true);
}

PCE_SCENERY void space_hull_draw(int16_t y,uint8_t flash,bool gone) {
    if(!space_hull_ready)return;
    video_scroll((uint16_t)-76,(uint16_t)-y);
    if(gone&&!hull_gone) {
        video_vdc(0,0);
        for(uint16_t i=0;i<2048;++i)video_vdc(2,PCE_BG_WORD>>4);
        hull_gone=1;
    }
    for(uint8_t i=0;i<64;++i) {
        uint16_t c=hull_palette[i];
        /* A hit warms the intact hull, keeping transparent/black pixels black. */
        if(flash&&(i&15))c=(c&0x1c7)|0x38;
        ((uint16_t*)buffer)[i]=c;
    }
    pce_vce_copy_palette(0,buffer,4);
}

PCE_SCENERY void space_screen_flash(uint8_t frames) {
    if(frames) {
        if(!space_flashing){pce_vce_copy_palette_to_ram(flash_palette,0,32);space_flashing=1;}
        for(uint16_t i=0;i<512;++i)((uint16_t*)buffer)[i]=0x1ff;
        pce_vce_copy_palette(0,buffer,32);
    } else if(space_flashing) {
        pce_vce_copy_palette(0,flash_palette,32);space_flashing=0;
    }
}

uint8_t dialog_corner_colour,dialog_corner_y;
PCE_SCENERY void dialog_corners(void) {
    extern vdc_sprite_t sat[2][64];
    extern uint8_t sat_page,sat_count,sprite_exact,sprite_occupancy[240];
    uint32_t a=pce_dialog_corners[pce_metrics.stage-1]+(uint16_t)dialog_corner_colour*544;
    arcade_read(2,a,buffer,32);pce_vce_copy_palette(31,buffer,1);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(a+32,0x4100,512);   /* nine panel characters fit below $4100 */
    int16_t x=24-(pce_scroll_x&7);uint8_t y=dialog_corner_y*8;
    for(uint8_t k=0;k<4;++k) {
        uint8_t sy=y+(k>=2?32:0);
        sat[sat_page][sat_count++]=(vdc_sprite_t){sy+64,x+(k&1?208:0)+32,0x208+k*2,VDC_SPRITE_FG|15};
        uint8_t lo=sprite_exact?sy:sy>>3,hi=sprite_exact?sy+16:(sy+16)>>3;
        for(uint8_t line=lo;line<hi;++line)++sprite_occupancy[line];
    }
}

PCE_SCENERY void dialog_palette_prepare(void) {
    uint32_t pal;
    extern const PceScene *video_scene_ptr;
    extern uint8_t herd_on;
    if(herd_on){arcade_read(2,video_scene_ptr->horse,buffer+512,32);return;}
    uint16_t id=pce_present_base[pce_metrics.stage-1][0];
    if(video_scene_ptr->nforeground)arcade_read(2,video_scene_ptr->foreground+4,&id,2);
    arcade_read(2,video_scene_ptr->sprites+(uint32_t)id*16+8,&pal,4);
    arcade_read(2,pal,buffer+512,32);
}
