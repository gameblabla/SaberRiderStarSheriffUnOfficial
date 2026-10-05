#include "scenery_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "race_pce.h"
#include "campaign_pce.h"

extern uint8_t buffer[2048];
uint8_t space_hull_ready,space_hull_top[24],space_hull_bottom[24];
uint16_t flash_palette[512] PCE_STAGE;
static uint16_t hull_palette[64] PCE_STAGE;
uint8_t space_flashing;
static uint8_t hull_lit;   /* the hull palette on the VCE: 0 plain, 1 warmed by a hit (written only when it changes) */
static uint8_t hull_gone;
volatile uint16_t pce_sky_far,pce_sky_near;

int16_t space_hull_x;uint16_t space_hull_char;
/* The hull's cells and the black playfield round them (the whole 64x32 BAT: the dialogue's cells and the beam's are wiped too). */
PCE_SCENERY void space_hull_bat(void) {
    video_vdc(0,0);
    for(uint16_t i=0;i<2048;++i)video_vdc(2,PCE_BG_WORD>>4);
    arcade_read(2,pce_boss_bg[6]+192,buffer,23*13*2);
    for(uint8_t y=0;y<13;++y) {
        video_vdc(0,(uint16_t)y*64);
        for(uint8_t x=0;x<23;++x)video_vdc(2,((uint16_t*)buffer)[(uint16_t)y*23+x]+(PCE_BG_WORD>>4));
    }
}
/* The nose cannon's beam, a stripe of cells left of the hull (BAT columns 54-63) at the nose's rows, drawn in the BG so that it moves with the
 * hull: 0 off, 1 the thin beam (rows 5-6), 2 the wide one (rows 4-7). The tiles are the last five of the hull's set (palette 4). */
PCE_SCENERY void space_beam(uint8_t width) {
    for(uint8_t row=4;row<8;++row) {
        uint16_t cell=0;   /* blank */
        if(width==1&&row==5)cell=space_hull_char;
        else if(width==1&&row==6)cell=space_hull_char+1;
        else if(width==2)cell=space_hull_char+(row==4?3:row==7?4:2);
        video_vdc(0,(uint16_t)row*64+54);
        for(uint8_t x=0;x<10;++x)video_vdc(2,cell?cell|0x4000:PCE_BG_WORD>>4);
    }
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
        video_wait();vce_copy_now(0,buffer,16);video_wait();
    }
    arcade_read(2,a+64,hull_palette,128);
    video_display(false);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(a+832,PCE_BG_WORD,count*32);
    space_hull_char=(PCE_BG_WORD>>4)+count-5;
    arcade_read(2,a+832+(uint32_t)count*32,buffer,32);pce_vce_copy_palette(4,buffer,1);   /* the beam's colours */
    space_hull_bat();
    pce_vce_copy_palette(0,hull_palette,4);
    pce_vce_set_color(255,0x1ff);   /* the ending dialogue still needs white BG glyphs */
    video_scroll((uint16_t)(-76-space_hull_x),(uint16_t)-60);
    space_flashing=hull_gone=0;space_hull_ready=1;hull_lit=0;
    video_wait();video_display(true);
}

PCE_SCENERY void space_hull_draw(int16_t y,uint8_t flash,bool gone) {
    if(!space_hull_ready)return;
    video_scroll((uint16_t)(-76-space_hull_x),(uint16_t)-y);
    if(gone&&!hull_gone) {
        video_vdc(0,0);
        for(uint16_t i=0;i<2048;++i)video_vdc(2,PCE_BG_WORD>>4);
        hull_gone=1;
    }
    if(hull_lit==(flash!=0))return;
    hull_lit=flash!=0;
    for(uint8_t i=0;i<64;++i) {
        uint16_t c=hull_palette[i];
        /* A hit blanks the intact hull white, keeping transparent/black pixels black. */
        if(flash&&(i&15))c=0x1ff;
        ((uint16_t*)buffer)[i]=c;
    }
    pce_vce_copy_palette(0,buffer,4);
}

PCE_SCENERY void space_screen_flash(uint8_t frames) {
    if(frames) {
        if(!space_flashing) {   /* the white is written once, in a vertical blank, and held */
            pce_vce_copy_palette_to_ram(flash_palette,0,32);space_flashing=1;
            for(uint16_t i=0;i<512;++i)((uint16_t*)buffer)[i]=0x1ff;
            pce_vce_copy_palette(0,buffer,32);
            vce_hold=1;   /* from here the palettes are kept in flash_palette, whoever writes them */
        }
    } else if(space_flashing) {
        vce_hold=0;
        pce_vce_copy_palette(0,flash_palette,32);space_flashing=0;
    }
}

/* Platform dialogue corners are ordinary cached sprites now (story_pce.c draws the two halves, which hold
 * only the four rounded corners): no reserved patterns, and shared sprite palette 31 stays the foreground's. */
