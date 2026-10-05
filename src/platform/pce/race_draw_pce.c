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
#define DRAW_CODE __attribute__((noinline,minsize,section(".ram_bank124.text")))
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
DRAW_CODE void race_draw(void) {
    base=pce_hud_base[1];
    overlay_call(0x77,project_entities);   /* the entities in the camera's frame (race_proj.c) */
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
        if(vis[k].kind>=8&&vis[k].kind<13){video_sprite_optional(PCE_CAR_EXPL+(dots>70?0:5)+vis[k].kind-8,vis[k].x,vis[k].y,false,16);continue;}
        if(vis[k].kind==7||vis[k].kind==13) {   /* the start / finish line's flag poles, one each side of the road (the right-hand one is the left one flipped) */
            uint16_t f=vis[k].f;
            video_sprite_optional(PCE_CAR_POLE+(f<125?5:f<165?4:f<225?3:f<310?2:f<430?1:0),vis[k].x,vis[k].y,vis[k].kind==13,16);
            continue;
        }
        uint8_t i=0;
        while(i<PCE_CAR_STEPS-1&&(uint16_t)pce_car_widths[i]*2<dots)++i;
        /* Baked perspective sizes use the cached assembly emitter. Scaling
         * every 16px piece in software consumed the floor's CPU budget. */
        video_sprite_optional(3+vis[k].kind*PCE_CAR_STEPS+i,vis[k].x,vis[k].y,false,16);
    }
    /* shots: the source's 8x8 orbs (blue the car's, red theirs) at their ground positions */
    for(uint8_t k=0;k<10;++k)if(bolt_ok&(1<<k)) {
        int16_t below=bolt_sy[k]-113;
        video_sprite_optional(PCE_CAR_ORB+(race_bolts[k].own?0:PCE_ORB_STEPS)+bolt_step[k],bolt_sx[k],bolt_sy[k]-(below>>3)-(below>>6),false,16);   /* 1400 / f without the division; big at the gun, smaller as it flies away */
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
    if(lap_banner&&(lap_banner>40||(lap_banner&8))) {   /* LAP 2/3 as the lap begins, FINAL LAP on the last (blinking as it goes) */
        uint8_t last=pce_campaign.lap>=3;
        put(last?PCE_H2_LAPMSG3:PCE_H2_LAPMSG2,256-(last?PCE_H2_LAPMSG3_W:PCE_H2_LAPMSG2_W)/2,52);
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
