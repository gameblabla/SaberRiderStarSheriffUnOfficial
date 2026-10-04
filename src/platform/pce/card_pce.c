#include "frontend_pce.h"
#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "campaign_pce.h"
#define CARD_CODE __attribute__((noinline,section(".ram_bank121.text")))
#define CARD_DATA __attribute__((section(".ram_bank121.rodata")))
extern volatile uint16_t pce_scroll_x;

/* The stage's title card (game.c title_draw), shown from the moment a stage is asked for through its disc read: black, an
 * amber rule above and below, "STAGE n" in amber, the name typing in, the sub-title following. The font is already in VRAM
 * (loader_font); the card stays up for the whole read (loader_scene), which writes Arcade RAM only. */
static const char s1[] CARD_DATA="STAGE 1",n1[] CARD_DATA="THE FRONTIER TOWN",t1[] CARD_DATA="OUTRIDERS IN THE STREETS";
static const char s2[] CARD_DATA="STAGE 2",n2[] CARD_DATA="THE ALL GALAXY GRAND PRIX",t2[] CARD_DATA="NEW BORDERLAND CIRCUIT";
static const char s3[] CARD_DATA="STAGE 3",n3[] CARD_DATA="HYPERJUMPER PASS",t3[] CARD_DATA="OUTRIDER SKY RAID";
static const char s4[] CARD_DATA="STAGE 4",n4[] CARD_DATA="THE RED PALM JUNGLE",t4[] CARD_DATA="OUTRIDER WATCHTOWERS";
static const char s5[] CARD_DATA="STAGE 5",n5[] CARD_DATA="THE CAVERN LABORATORY",t5[] CARD_DATA="OUTRIDER HIDEOUT";
static const char s6[] CARD_DATA="STAGE 6",n6[] CARD_DATA="POWER STRIDE",t6[] CARD_DATA="RENEGADES AT THE YUMA OUTPOST";
static const char s7[] CARD_DATA="STAGE 6 - FINAL PHASE",n7[] CARD_DATA="THE BATTLE CRUISER",t7[] CARD_DATA="OUTRIDER SPACE";
static const char *const card_text[7][3] CARD_DATA={{s1,n1,t1},{s2,n2,t2},{s3,n3,t3},{s4,n4,t4},{s5,n5,t5},{s6,n6,t6},{s7,n7,t7}};
#define RULE_TILE 0x100   /* a character of two amber scanlines, built in free VRAM at word 0x1000 */

CARD_CODE static void put(uint8_t row,const char *text,uint8_t palette,uint8_t shown) {
    uint8_t length=0;while(text[length])++length;
    uint8_t column=(40-length)/2;
    video_vdc(0,(uint16_t)row*64+column);
    for(uint8_t k=0;k<length;++k)video_vdc(2,((uint16_t)palette<<12)|((PCE_FONT_WORD>>4)+(k<shown?text[k]-32:0)));
}
CARD_CODE void frontend_card(void) {
    uint8_t stage=pce_control.stage;if(stage<1||stage>7)stage=1;
    const char *const *text=card_text[stage-1];
    video_display(false);pce_raster_enabled=0;
    video_mode_ui();
    pce_vce_set_color(0,0);
    pce_vce_set_color(13*16+15,0x16e);pce_vce_set_color(14*16+15,0x178);pce_vce_set_color(15*16+15,0x1ff);
    pce_vce_set_color(14*16+1,0x178);
    uint16_t blank=0xf000|(PCE_FONT_WORD>>4);
    video_vdc(0,0);
    for(uint16_t k=0;k<64*32;++k)video_vdc(2,blank);
    video_vdc(0,0x1000);
    for(uint8_t k=0;k<16;++k)video_vdc(2,k<2?0x00ff:0);
    for(uint8_t r=0;r<2;++r) {
        video_vdc(0,(uint16_t)(r?18:11)*64);
        for(uint8_t x=0;x<40;++x)video_vdc(2,0xe000|RULE_TILE);
    }
    video_sat_begin();video_sat_end();
    pce_scroll_x=0;
    video_display(true);
    uint8_t name=0,length=0;while(text[1][length])++length;
    put(12,text[0],14,255);
    for(;name<=length;++name) {
        put(14,text[1],15,name);
        video_wait();video_wait();
    }
    put(16,text[2],13,255);
    video_wait();
}
