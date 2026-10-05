#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "assets.h"
#define RACE_SIN_SECTION ".ram_bank124.rodata"
#include "race_math.h"
#include "race_pce.h"
PCE_BOSS void race_briefing_frame(void) {
    overlay_call(0x79,race_frame);
    video_wait();
}
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
static Visible vis[18];static uint8_t nvis;static int8_t shown_c,shown_s,shown_cl,shown_sl;
/* (a * b) >> 7 for |a| < 4096 and |b| <= 127 without 32-bit arithmetic: the high and the low byte of a apart */
DRAW_CODE static int16_t mulq(int16_t a,int8_t b) {
    int8_t high=a>>8;uint8_t low=a;
    return (int16_t)(high*b*2+(int16_t)((int16_t)low*b>>7));
}
/* a * b for int16 a and int8 b, in 32 bits */
DRAW_CODE static int32_t mulw(int16_t a,int8_t b) {
    int8_t high=a>>8;uint8_t low=a;
    return ((int32_t)(int16_t)(high*b)<<8)+(int16_t)((int16_t)low*b);
}
/* The ground point in the camera's frame: f ahead, l to the right. The screen row follows from the distance (horizon
 * 113 + 10080 / f); a car's width in dots is 1.17 x the rows below the horizon; the offset from the middle is
 * l * 421 / f = l * rows / 23.9, taken as l * (rows * 0.668 in Q4) >> 4 so that it stays in 16 bits. */
DRAW_CODE static bool project_point(int16_t wx,int16_t wy,int16_t *sx,int16_t *row,int16_t *depth) {
    int16_t rx=wrapdiff(wx,pce_control.x),ry=wrapdiff(wy,pce_control.y);
    if(rx>1900||rx<-1900||ry>1900||ry<-1900)return false;
    int16_t f=mulq(rx,shown_c)+mulq(ry,shown_s);
    if(f<40||f>560)return false;   /* beyond 560 units (road_pce.c FAR_F) the road is lost in the haze: nothing is drawn there */
    /* the side offset in quarter units, from the camera's Q14 sine and cosine: whole units moved a car four dots at a time near the camera,
     * out of step with the road under it */
    int16_t l=(int16_t)((((mulw(ry,shown_c)-mulw(rx,shown_s))<<7)+mulw(ry,shown_cl)-mulw(rx,shown_sl))>>12);
    if(l>4*f||l<-4*f)return false;
    uint16_t below=10080u/(uint16_t)f;
    *row=113+below;*depth=f;
    *sx=256+(int16_t)(l*(int16_t)((below*171)>>8)>>6);
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
    shown_c=cam_c;shown_s=cam_s;shown_cl=cam_cl;shown_sl=cam_sl;
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
        int16_t t=tilt;   /* Q4, as mode7.c's steering lean x 16 */
        /* A spin-out is one whole turn, easing out (mode7.c: angle = 360 p (2 - p) with p the elapsed share of the 36 steps),
         * shown as the nearest of the baked poses; pose 0 (and the full turn) is the upright car. */
        uint8_t pose=0;
        if(spin) {
            uint16_t p=(uint16_t)(36-(spin>36?36:spin))*7;   /* 0..252 of 256 */
            pose=(uint8_t)((((p*(512-p))>>8)*PCE_CAR_SPIN_FRAMES+128)>>8);
            if(pose>=PCE_CAR_SPIN_FRAMES)pose=0;
        }
        /* The car slides sideways with its lean (mode7.c: 1.2 x the lean, in pixels; two dots each here). */
        int16_t x=256+((t*5)>>5);
        /* the afterburner, first in the SAT so it burns in front of the car: a flame over each exhaust nozzle (the offsets, in
         * dots from the car's centre, follow the five poses) */
        if(boost_on&&!pose) {
            static const int8_t NOZZLE[5][2]={{-20,34},{-25,31},{-30,28},{-33,23},{-36,19}};
            uint8_t pose5=t<-80?0:t<-24?1:t>80?4:t>24?3:2,frame=(phase_t/3)&3;
            for(uint8_t n=0;n<2;++n)video_sprite_optional(PCE_CAR_TURBO+((frame+2*n)&3),x+NOZZLE[pose5][n],195,false,16);
        }
        if(pose) {
            video_sprite(PCE_CAR_SPIN+(pose-1)*2,x,215,false,16);
            video_sprite(PCE_CAR_SPIN+(pose-1)*2+1,x,215,false,16);
        } else {
            uint16_t id=t<-80?PCE_CAR_STEER:t<-24?PCE_CAR_STEER+1:t>80?PCE_CAR_STEER+3:t>24?PCE_CAR_STEER+2:3+PCE_CAR_STEPS-1;
            video_sprite(id,x,215,false,16);
        }
    }
    for(uint8_t k=0;k<nvis;++k) {
        uint16_t rows=vis[k].y-113,dots=rows+(rows>>3)+(rows>>5);   /* 11776 / f */
        uint8_t i=0;
        while(i<PCE_CAR_STEPS-1&&(uint16_t)pce_car_widths[i]*2<dots)++i;
        /* Baked perspective sizes use the cached assembly emitter. Scaling
         * every 16px piece in software consumed the floor's CPU budget. */
        video_sprite_optional(3+vis[k].kind*PCE_CAR_STEPS+i,vis[k].x,vis[k].y,false,16);
    }
    /* shots: the source's 8x8 orbs (blue the car's, red theirs) at their ground positions */
    for(uint8_t k=0;k<10;++k)if(race_bolts[k].t) {
        int16_t sx,row,f;
        if(project_point(race_bolts[k].x,race_bolts[k].y,&sx,&row,&f))video_sprite_optional(race_bolts[k].own?0:1,sx,row-(1400/f),false,16);
    }
    /* HUD: the car's damage bar and spare cars, the turbo bar (no speedometer), then what the phase has to say */
    uint16_t hp_frac=(uint16_t)car_hp*64/car_max;   /* 16-bit throughout: the 32-bit multiply and divide cost the road its updates */
    put(PCE_H2_ICON,10,12);
    bar(46,16,4,hp_frac,car_hp*2>car_max?0:car_hp*4>car_max?1:2);
    put(PCE_H2_X,10,32);number(26,32,pce_campaign.lives,1,0);
    bar(10,206,4,boost>>2,boost_locked||boost<=64?2:boost_on||boost>128?3:1);   /* orange in use or high, yellow in between, red locked out or nearly flat */
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
        /* Two rows centred on the screen: GAP, the distance and M above, the bar under them. A row may carry 16 sprite pieces and
         * the hull bar already takes six of the first: the old single row (label, bar, number and M = 16 more) lost its digits. */
        uint16_t span=gap_dist>4200?4200:gap_dist;
        uint16_t fill=span<260?96:96-(span-260)/41;
        put(PCE_H2_GAP,184,8);number(250,8,gap_dist>9999?9999:gap_dist,4,0);put(PCE_H2_M,310,8);
        bar(208,30,6,fill,3);
    } else if(rphase==P_BOSS&&boss.state<2) {
        /* the leader's hull, a red bar centred under the HUD row (it had none) */
        bar(176,30,10,boss.hp*3,2);   /* 3 dots a hit point (30-50 points a difficulty) */
    }
    video_sat_end();
}
