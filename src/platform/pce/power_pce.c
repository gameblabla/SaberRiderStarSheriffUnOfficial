#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "play_pce.h"
#include "scenery_pce.h"
#include "ui_pce.h"
#include "audio_pcm.h"
#include <string.h>
#include "loader_pce.h"
#include "sgx_pce.h"
#define POWER_CODE PCE_BOSS   /* bank $7b */
/* The hero power's cut-in (no video: the PC's clips are replaced by this). The world holds still, dims, and a blue wave band opens across the
 * middle of the screen, wiped in across the whole width, its curling crests flowing, then the hero's anime portrait slides in at the left, the
 * hero's name (a sprite: white with a black outline, over the wave) at the right, and the screen burns out to white and the power lands (combat_tick / space_tick, pce_power_land).
 *
 * The wave is BG characters (VRAM $4000-$43ff: the dialogue's panel characters and the first of the font's glyphs, which the cut-in puts back) in
 * palette 15, written over the BAT rows 7-20 (the screen's 33 columns; band row r is character row r % 6 of a field that repeats every 64 dots across and 48 down; the
 * band's first and last rows carry a black outline). The crests move by rotating the 14 ramp colours through palette 15 (power_wave in
 * build_assets.py). The portrait is three cached sprites (pwr hero) and the name a fourth, sharing the hero's palette. The dimming and the white-out
 * work from a snapshot of every palette in `buffer` (ui_fade / ui_lift), taken when the cut-in begins; the dialogue's own panel restore is
 * not used, the BG cells go back from the column cache (as video_panel_restore_*) while the screen is white. */
extern uint8_t buffer[2048];
extern volatile uint16_t pce_scroll_x,pce_scroll_y;
extern const PceScene *video_scene_ptr;
extern uint16_t columns[33][30],flight_clock;
uint8_t pce_power_land PCE_WORK;   /* the cut-in is over: the strike lands in the next tick */
#define BAND_ROWS 14
#define COLS 33          /* the screen's 32 columns and the one the sub-character scroll uncovers */
#define OPEN_AT 5       /* the step the band starts wiping in from the left (three columns a step) */
#define PORTRAIT_AT 20
#define NAME_AT 28
#define FLASH_AT 96
#define END_AT 104
/* Columns x0..x1-1 of the screen's 33 (from BAT column col0) of every band row, each row's characters. */
POWER_CODE static void band_cells(uint8_t x0,uint8_t x1,uint8_t col0,uint8_t row0) {
    for(uint8_t r=0;r<BAND_ROWS;++r) {
        uint8_t kind=r==0?6:r==BAND_ROWS-1?7:r%6;
        uint16_t base=(uint16_t)((row0+r)&31)*64;
        pce_cpu_irq_disable();
        for(uint8_t x=x0;x<x1;++x) {
            uint8_t c=(col0+x)&63;
            if(x==x0||!c) {
                uint16_t address=base+c;
                *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
                *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
            }
            uint16_t cell=0xf000|(0x400+kind*8+(c&7));
            *IO_VDC_DATA_LO=cell;*IO_VDC_DATA_HI=cell>>8;
        }
        pce_cpu_irq_enable();
    }
}
/* The BG cells the band covered, from the column cache and the map (as presentation_pce.c panel_prepare_body / panel_apply_body, for the band's
 * 14 rows; only the platform stages have a column cache). The window is the screen's 33 columns. */
POWER_CODE static void band_restore(uint8_t col0,uint8_t row0) {
    const PceScene *sc=video_scene_ptr;
    uint16_t *words=(uint16_t*)(buffer+1024);   /* (the palette snapshot is in the first 1024 bytes) */
    if(pce_sgx_gameplay()&&pce_metrics.stage!=1&&pce_metrics.stage!=3) {
        for(uint16_t i=0;i<COLS*BAND_ROWS;++i)words[i]=PCE_FONT_WORD>>4;
    } else {
    uint32_t map=sc->map;
#ifdef PCE_SGX
    if(pce_sgx_gameplay()&&(pce_metrics.stage==1||pce_metrics.stage==3))map=sc->sgx_map;
#endif
    for(uint8_t x=0;x<COLS;++x) {
        uint16_t world=(uint16_t)(pce_scroll_x>>3)+x;
        uint8_t raw[BAND_ROWS*3];
        arcade_read(1,map+(uint32_t)(world%sc->cols)*90+(uint16_t)row0*3,raw,BAND_ROWS*3);
        for(uint8_t row=0;row<BAND_ROWS;++row)
            words[x*BAND_ROWS+row]=(PCE_BG_WORD>>4)+columns[world%33][row0+row]+((uint16_t)raw[row*3+2]<<12);
    }
    }
    uint16_t control=*(volatile uint16_t *)0x20f3;
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = (control>>8)|0x10;   /* the address steps 64 words: down a column */
    for(uint8_t x=0;x<COLS;++x) {
        uint16_t address=(uint16_t)row0*64+((col0+x)&63);
        *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
        *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
        for(uint8_t row=0;row<BAND_ROWS;++row){uint16_t w=words[x*BAND_ROWS+row];*IO_VDC_DATA_LO=w;*IO_VDC_DATA_HI=w>>8;}
    }
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = control>>8;
    pce_cpu_irq_enable();
}
POWER_CODE void power_frame(void) {
    uint16_t t=pce_campaign.timer++;
    uint8_t stage=pce_metrics.stage,col0=pce_scroll_x>>3,row0=7+(pce_scroll_y>>3);
    uint32_t wave=pce_power_wave[stage-1];
    if(!t) {
        /* End the space flash's palette ownership before the cut-in starts
         * fading; otherwise its writes only update the held flash palette. */
        if(stage==7)overlay_call(0x78,space_screen_flash_end);
        if(stage<6)pce_scroll_y=0;   /* (a shake left over from the last frame) */
        row0=7;
        ui_fade(8);   /* every palette, into the snapshot */
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(wave+28,0x4000,2048);
        video_sat_begin();video_sat_end();
    }
    if(t>=1&&t<=4)ui_fade((uint8_t)t);
    if(t>=OPEN_AT&&t<OPEN_AT+11) {   /* the wave wipes in from the left, across the whole width */
        uint8_t x0=(uint8_t)(t-OPEN_AT)*3,x1=x0+3;if(x1>COLS)x1=COLS;
        band_cells(x0,x1,col0,row0);
    }
    if(t>=OPEN_AT&&t<FLASH_AT) {   /* the crests flow */
        uint16_t ramp[14],palette[16];
        arcade_read(2,wave,ramp,28);
        uint8_t shift=(uint8_t)t;while(shift>=14)shift-=14;
        for(uint8_t i=0;i<14;++i){uint8_t j=i+shift;if(j>=14)j-=14;palette[1+i]=ramp[j];}
        palette[0]=palette[15]=0;
        pce_vce_copy_palette(15,palette,1);
        if(t==FLASH_AT-1)memcpy(buffer+15*32,palette,32);   /* the white-out whitens the band too */
    }
    if(t>=PORTRAIT_AT&&t<FLASH_AT) {
        uint8_t p=t-PORTRAIT_AT;if(p>16)p=16;
        uint16_t e=(uint16_t)p*(32-p);   /* 0..256: ease out */
        int16_t x=-64+(int16_t)((76*e)>>8);
        uint16_t pb=pce_power_base[stage-1],base=pb+3*pce_control.hero;
        video_sat_begin();
        if(t>=NAME_AT) {   /* the name slides in from the right, centred on the right half of the band */
            uint8_t q=t-NAME_AT;if(q>16)q=16;
            uint16_t f=(uint16_t)q*(32-q);
            static const uint8_t name_w[4]={74,116,74,60};
            uint16_t target=170-name_w[pce_control.hero]/2;
            video_sprite(pb+12+pce_control.hero,256-(int16_t)(((256-target)*f)>>8),103,false,16);
        }
        for(uint8_t k=0;k<3;++k)video_sprite(base+k,x,32+64*k,false,16);
        video_sat_end();
        if(t==PORTRAIT_AT)audio_pcm_power_intro();
    }
    if(t==FLASH_AT){video_sat_begin();video_sat_end();}
    if(t>=FLASH_AT&&t<END_AT&&!(t&1)) {   /* burning out to white */
        ui_lift_level=(uint8_t)(2*((t-FLASH_AT)/2+1));if(ui_lift_level>7)ui_lift_level=7;
        overlay_call(0x72,ui_lift);
    }
    if(t==END_AT-1) {   /* under the white: the font and the BG cells go back */
        if(stage==7) {
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            arcade_vram(pce_dialog_original_font[6],PCE_FONT_WORD,3072);
            if(space_hull_ready)overlay_call(0x78,space_hull_bat);
            else{video_restore();video_background(flight_clock>>3);}   /* the nebula's columns, under the white */
        } else {
            pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
            arcade_vram(pce_dialog_original_font[stage-1],PCE_FONT_WORD,3072);
            band_restore(col0,7);
        }
    }
    if(t==END_AT) {
        ui_fade(0);   /* the palettes as they were */
        if(!(pce_sgx_gameplay()&&stage==7)) {
            uint32_t palette=video_scene_ptr->pal;
#ifdef PCE_SGX
            if(pce_sgx_gameplay()&&(stage==1||stage==3))palette=video_scene_ptr->sgx_pal;
#endif
            arcade_read(2,palette+15*32,buffer+1024,32);
            pce_vce_copy_palette(15,buffer+1024,1);
        }
        pce_vce_set_color(255,0x1ff);
        pce_campaign.state=CAM_PLAY;pce_campaign.timer=0;pce_power_land=1;
    }
}
