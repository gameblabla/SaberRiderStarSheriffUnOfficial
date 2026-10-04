#include "campaign_pce.h"
#include "video_pce.h"
#include "overlay_pce.h"
#include "assets.h"
#include "hud_pce.h"
/* Sprite HUDs of the stages that have none of the platform HUD's own: pre-rendered pieces (tools/pce/hudart.py) placed
 * at fixed spots, so nothing is drawn on background cells (those left black boxes on the art). */
Hud6 hud6;
static uint16_t base;
PCE_HUD static void put(uint16_t offset,int16_t x,int16_t y,bool flip) {
    video_sprite_optional(base+offset,x,y,flip,16);
}
/* A horizontal bar of up to 2 pieces: `pixels` filled px of a 22 px bar starting at x. Colour 0-3 = green yellow red orange. */
PCE_HUD static void bar24(int16_t x,int16_t y,uint8_t pixels,uint8_t color) {
    if(pixels>22)pixels=22;
    uint16_t first=PCE_H6_BAR_GREEN_1+(uint16_t)color*16-1;
    if(pixels)put(first+(pixels>16?16:pixels),x,y,false);
    if(pixels>16)put(first+(pixels-16),x+16,y,false);
}
PCE_HUD static void digit(int16_t x,int16_t y,uint8_t color,uint8_t value) {
    put(PCE_H6_DIGIT_WHITE_0+(uint16_t)color*10+value,x,y,false);
}
/* The Ramrod cockpit (ramrod.c render_monitors, render_aim): the radar's dots, the status monitor's bars and digits,
 * the crosshair and an arrow at the edge for every threat outside the view. Monitors are 52x33 at (33,23) and (174,23). */
PCE_HUD void hud6_draw(void) {
    base=pce_hud_base[5];
    const Hud6 *h=&hud6;
    /* radar: 1 px = 60 units forward, centre (59,41); dots clamp to the screen's edge like the source */
    for(uint8_t k=0;k<3;++k)if(h->dot_on[k]) {
        int16_t x=59+h->dot_x[k],y=41-h->dot_y[k];
        if(x<34)x=34;if(x>81)x=81;if(y<24)y=24;if(y>52)y=52;
        put(h->dot_blink[k]?PCE_H6_DOT_WHITE:h->dot_kind[k]==2?PCE_H6_DOT_GOLD:h->dot_kind[k]==1?PCE_H6_DOT_RED:PCE_H6_DOT_GREEN,x,y,false);
    }
    /* status: ARM and GUN bars, wave, enemies left, lives */
    uint8_t armor=h->armor,color=armor>50?0:armor>25?1:2;
    bool flash=armor<30&&((h->clock>>3)&1);
    if(!flash)bar24(201,26,(uint8_t)((uint16_t)armor*22/100),color);
    bar24(201,36,(uint8_t)((uint16_t)h->heat*22/100),h->hot||h->heat>=70?2:3);
    digit(184,46,1,h->wave);
    digit(194,46,2,h->left);
    digit(219,46,0,h->lives);
    /* aim */
    put(h->locked?PCE_H6_CROSS_RED:PCE_H6_CROSS_GREEN,128,94,false);
    for(uint8_t k=0;k<3;++k)if(h->threat[k]) {
        bool right=h->threat[k]>1;
        put(h->danger[k]&&((h->clock>>2)&1)?PCE_H6_CHEVRON_DANGER:PCE_H6_CHEVRON,right?244:6,118+k*8,right);
    }
    /* banners: the wave's title as it begins */
    if(h->banner) {
        uint16_t id=PCE_H6_WAVE1+h->banner-1;
        int16_t width=PCE_H6_WAVE1_W;
        put(id,128-width/2,64,false);
        if(h->banner==3&&h->banner_t>60)put(PCE_H6_WARNING,128-PCE_H6_WARNING_W/2,96,false);
    }
}

/* One frame of the cockpit: the arms, the mechs and the HUD. */
PCE_HUD void mech_draw(void) {
    static uint8_t frame_clock;
    video_background(0);video_sat_begin();
    uint16_t arm=PCE_MECH_ARM+(punch_cd>30?9:0);
    video_sprite(arm,0,0,false,16);video_sprite(arm,256,0,true,16);
    if(gun_cd>4)video_sprite(0,128,94,false,16);
    Hud6 *h=&hud6;
    for(uint8_t k=0;k<3;++k)h->dot_on[k]=h->threat[k]=0;
    int8_t target=-1;uint16_t nearest=2000;
    for(uint8_t k=0;k<3;++k)if(mechs[k].on) {
        Mech *m=&mechs[k];
        int16_t dx=m->x-aim;
        bool attack=m->clock%120>78;
        /* The mech's apparent width follows 1/distance; the nearest baked width at or above it is drawn with its 16 px
         * slices pulled together to the exact width (sprite_generic's scale), so a mech grows smoothly. */
        uint16_t width=m->distance<180?80:(uint16_t)(14400UL/m->distance);
        if(width<20)width=20;
        uint8_t step=0;while(step<PCE_MECH_STEPS-1&&pce_mech_sizes[step]<width)++step;
        uint8_t size=pce_mech_sizes[step],scale=(uint8_t)(((uint16_t)width*16+size/2)/size);
        if(scale>16)scale=16;if(scale<12)scale=12;
        uint8_t pose=m->distance<180&&m->clock%60>40?6:attack?4:(m->clock/12)%4;
        uint16_t id=3+(uint16_t)m->variant*(PCE_MECH_STEPS*8)+step*8+pose;
        visible[k]=video_sprite_optional(id,m->x+128-aim,164,false,scale);
        if(dx<0)dx=-dx;
        if(dx<32&&m->distance<nearest){target=k;nearest=m->distance;}
        h->dot_on[k]=1;h->dot_kind[k]=m->variant;h->dot_blink[k]=attack;
        h->dot_x[k]=(int8_t)((m->x-aim)/5);
        h->dot_y[k]=(int8_t)(m->distance/50>25?25:m->distance/50);
        if(m->x<(int16_t)aim-128)h->threat[k]=1;else if(m->x>(int16_t)aim+128)h->threat[k]=2;
        h->danger[k]=attack;
    }
    h->armor=pce_metrics.hp>100?100:pce_metrics.hp;h->heat=heat>100?100:heat;h->hot=overheated;
    h->wave=pce_campaign.wave+1;h->lives=pce_campaign.lives;
    h->left=(pce_campaign.wave==0?4:pce_campaign.wave==1?6:8)-killed;h->locked=target>=0;
    h->banner=wave_clock<110?pce_campaign.wave+1:0;h->banner_t=wave_clock;h->clock=(uint8_t)frame_clock++;
    overlay_call(0x7c,hud6_draw);
    video_sat_end();
    pce_metrics.player_x=aim;pce_metrics.player_y=heat;
}

Hud7 hud7;
PCE_HUD static void put7(uint16_t offset,int16_t x,int16_t y) {
    video_sprite_optional(pce_hud_base[6]+offset,x,y,false,16);
}
/* Shield cells, labels, lives, gun power pips, torpedoes, hero-power stars and the cruiser's hull bar. */
PCE_HUD void hud7_draw(void) {
    const Hud7 *h=&hud7;
    uint8_t hp=h->hp>4?4:h->hp;
    bool blink=hp==1&&((h->clock>>3)&1);
    uint8_t c0=hp>0&&!blink,c1=hp>1,c2=hp>2,c3=hp>3;
    put7(PCE_H7_CELLS_TOP_0+(c3|(c2<<1)),3,8);
    put7(PCE_H7_CELLS_BOT_0+(c1|(c0<<1)),3,26);
    put7(PCE_H7_RAMROD,19,6);
    put7(PCE_H7_XLIFE,19,16);
    put7(PCE_H7_DIGIT_WHITE_0+(h->lives/10)%10,26,16);
    put7(PCE_H7_DIGIT_WHITE_0+h->lives%10,33,16);
    put7(PCE_H7_PWR,19,26);put7(PCE_H7_TRP,19,36);put7(PCE_H7_SPC,19,46);
    for(uint8_t i=0;i<3;++i)put7(i<h->power?PCE_H7_PIP_ON:PCE_H7_PIP_OFF,48+i*9,28);
    for(uint8_t i=0;i<h->bombs&&i<5;++i)put7(PCE_H7_TORP,52+i*7,40);
    for(uint8_t i=0;i<h->items&&i<2;++i)put7(PCE_H7_STAR,50+i*9,50);
    if(h->boss_on) {
        put7(PCE_H7_CRUISER,108,4);

    }
}
