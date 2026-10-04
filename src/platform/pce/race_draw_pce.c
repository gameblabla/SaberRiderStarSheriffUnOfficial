#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "assets.h"
#define RACE_SIN_SECTION ".ram_bank124.rodata"
#include "race_math.h"
#include "race_pce.h"
PCE_HUD void race_briefing_frame(void) {
    uint16_t commits=pce_metrics.floor_commits;
    do {overlay_call(0x79,race_frame);} while(pce_metrics.floor_commits==commits);
    video_wait();
}
#include "floor_pce.h"
/* The race's sprites and HUD. The screen runs at the 512-dot clock, so every x here is in dots (256 is the middle) and
 * every picture was stretched to twice its width at build time (tools/pce/hudart.py, build_assets.py). Cars are drawn
 * far to near from the camera's frame: its focal length is 421 dots, its horizon row 113, and a car at a distance f is
 * 128*92/f dots wide (the player's own, at 92, is 128). */
#define DRAW_CODE PCE_HUD
static uint16_t base;
DRAW_CODE static void put(uint16_t offset,int16_t x,int16_t y) {
    video_sprite_optional(base+offset,x,y,false,16);
}
/* digits at a 16-dot pitch; colour 0 white, 1 gold, 2 pink */
DRAW_CODE static void number(int16_t x,int16_t y,uint16_t value,uint8_t digits,uint8_t color) {
    uint16_t divisor=digits==4?1000:digits==3?100:digits==2?10:1;
    for(uint8_t k=0;k<digits;++k) {
        put(PCE_H2_DIGIT_WHITE_0+(uint16_t)color*10+(value/divisor)%10,x+k*14,y);
        divisor/=10;if(!divisor)divisor=1;
    }
}
/* A bar of up to `pieces` 16-dot pieces, `fill` dots of it filled, in colour 0-3 (green yellow red orange). */
DRAW_CODE static void bar(int16_t x,int16_t y,uint8_t pieces,uint16_t fill,uint8_t color) {
    for(uint8_t k=0;k<pieces&&fill>(uint16_t)k*16;++k) {
        uint16_t f=fill-k*16;if(f>16)f=16;
        put(PCE_H2_BAR_GREEN_1+(uint16_t)color*16+f-1,x+k*16,y);
    }
}
typedef struct {int16_t x,y;uint16_t f;uint8_t kind;} Visible;
static Visible vis[18];static uint8_t nvis;static int8_t shown_c,shown_s;
/* (a * b) >> 7 for |a| < 4096 and |b| <= 127 without 32-bit arithmetic: the high and the low byte of a apart */
DRAW_CODE static int16_t mulq(int16_t a,int8_t b) {
    int8_t high=a>>8;uint8_t low=a;
    return (int16_t)(high*b*2+(int16_t)((int16_t)low*b>>7));
}
/* The ground point in the camera's frame: f ahead, l to the right. The screen row follows from the distance (horizon
 * 113 + 10080 / f); a car's width in dots is 1.17 x the rows below the horizon; the offset from the middle is
 * l * 421 / f = l * rows / 23.9, taken as l * (rows * 0.668 in Q4) >> 4 so that it stays in 16 bits. */
DRAW_CODE static bool project_point(int16_t wx,int16_t wy,int16_t *sx,int16_t *row,int16_t *depth) {
    int16_t rx=wrapdiff(wx,floor_shown_x),ry=wrapdiff(wy,floor_shown_y);
    if(rx>1900||rx<-1900||ry>1900||ry<-1900)return false;
    int16_t f=mulq(rx,shown_c)+mulq(ry,shown_s);
    if(f<40)return false;
    int16_t l=mulq(ry,shown_c)-mulq(rx,shown_s);
    if(l>f||l<-f)return false;
    uint16_t below=10080u/(uint16_t)f;
    *row=113+below;*depth=f;
    *sx=256+(int16_t)(l*(int16_t)((below*171)>>8)>>4);
    return true;
}
DRAW_CODE static void add(uint8_t kind,int16_t wx,int16_t wy) {
    int16_t sx,row,f;
    if(nvis>=18||!project_point(wx,wy,&sx,&row,&f))return;
    if(sx<-60||sx>572)return;
    uint8_t at=nvis;
    while(at&&vis[at-1].f>(uint16_t)f){vis[at]=vis[at-1];--at;}
    vis[at]=(Visible){sx,row,(uint16_t)f,kind};++nvis;
}
DRAW_CODE void race_draw(void) {
    base=pce_hud_base[1];
    shown_c=cosine((uint16_t)floor_shown_heading<<9);shown_s=sine((uint16_t)floor_shown_heading<<9);
    nvis=0;
    for(uint8_t k=0;k<7;++k)if(rv[k].hp)add(rv[k].kind,rv[k].x,rv[k].y);
    for(uint8_t k=0;k<6;++k)if(mines[k].t)add(6,mines[k].x,mines[k].y);
    if(rphase>=P_PURSUIT) {
        for(uint8_t k=0;k<2;++k)if(escort[k].hp)add(1,escort[k].x,escort[k].y);
        if(boss.state<3)add(2,boss.x,boss.y);
    }
    video_sat_begin();
    /* the car: hard left .. hard right from the steering lean, blinking while it is hurt */
    if(!hurt||(race_time&4)||rphase!=P_RACE) {
        int16_t t=tilt;
        uint16_t id=t<-4?PCE_CAR_STEER:t<-1?PCE_CAR_STEER+1:t>4?PCE_CAR_STEER+3:t>1?PCE_CAR_STEER+2:3+PCE_CAR_STEPS-1;
        video_sprite(id,256,215,false,16);
    }
    for(uint8_t k=0;k<nvis;++k) {
        uint16_t rows=vis[k].y-113,dots=rows+(rows>>3)+(rows>>5);   /* 11776 / f */
        uint8_t i=0;
        while(i<PCE_CAR_STEPS-1&&(uint16_t)pce_car_widths[i]*2<dots)++i;
        uint16_t baked=(uint16_t)pce_car_widths[i]*2,scale=(dots*16+baked/2)/baked;
        if(scale>16)scale=16;if(scale<6)scale=6;
        video_sprite_optional(3+vis[k].kind*PCE_CAR_STEPS+i,vis[k].x,vis[k].y,false,(uint8_t)scale);
    }
    /* shots: the source's 8x8 orbs (blue the car's, red theirs) at their ground positions */
    for(uint8_t k=0;k<6;++k)if(race_bolts[k].t) {
        int16_t sx,row,f;
        if(project_point(race_bolts[k].x,race_bolts[k].y,&sx,&row,&f))video_sprite_optional(race_bolts[k].own?0:1,sx,row-(1400/f),false,16);
    }
    /* HUD: the car's damage bar and spare cars, the turbo bar and the speed, then what the phase has to say */
    uint16_t hp_frac=(uint16_t)((uint32_t)car_hp*64/car_max);
    put(PCE_H2_ICON,10,12);
    bar(46,16,4,hp_frac,car_hp*2>car_max?0:car_hp*4>car_max?1:2);
    put(PCE_H2_X,10,32);number(26,32,pce_campaign.lives,1,0);
    bar(10,206,4,(uint16_t)((uint32_t)boost*64/255),boost_on||boost>128?3:2);
    number(436,206,(uint16_t)((int32_t)(speed>0?speed:0)*3/5),3,0);
    if(rphase<=P_FINISH) {
        uint8_t lap=pce_campaign.lap>3?3:pce_campaign.lap;
        number(440,10,lap,1,0);put(PCE_H2_SLASH3,454,10);
        uint8_t rank=pce_campaign.rank<1?1:pce_campaign.rank>8?8:pce_campaign.rank;
        put(PCE_H2_ORD1+rank-1,440,26);
    }
    if(rphase==P_COUNT) {
        uint8_t n=3-phase_t/60;if(n>3)n=3;if(n<1)n=1;
        put(PCE_H2_C3+(3-n),240,80);
    } else if(rphase==P_RACE&&phase_t<60)put(PCE_H2_GO,256-PCE_H2_GO_W/2,80);
    else if(rphase==P_FINISH) {
        put(PCE_H2_FINISH,256-PCE_H2_FINISH_W/2,64);
        uint8_t rank=finish_rank<1?1:finish_rank>8?8:finish_rank;
        put(PCE_H2_ORD1+rank-1,200,84);put(PCE_H2_PLACE,200+PCE_H2_ORD1_W+8,84);
    } else if(rphase==P_PURSUIT) {
        uint16_t span=gap_dist>4200?4200:gap_dist;
        uint16_t fill=span<260?96:(uint16_t)(96-(uint32_t)(span-260)*96/3940);
        put(PCE_H2_GAP,180,14);bar(214,18,6,fill,3);
        number(320,12,gap_dist>9999?9999:gap_dist,4,0);put(PCE_H2_M,376,12);
    } else if(rphase==P_BOSS) {
        bar(214,18,6,(uint16_t)((uint32_t)boss.hp*96/boss.hp_max),2);
    }
    video_sat_end();
}
