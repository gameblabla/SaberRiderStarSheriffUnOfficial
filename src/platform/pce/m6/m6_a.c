#pragma clang section text=".ram_bank128.text" rodata=".ram_bank128.rodata" data=".ram_bank128.data" bss=".ram_bank128.bss"
#include "m6_common.h"
#include "arcade_pce.h"
#define M6_SECTION ".ram_bank128.rodata"
#define M6_RCP_ONLY
#include "m6.h"
/* Image A: Ramrod itself (ramrod.c player_control): the heading, walking and side-stepping, the shoulder guns (they overheat), the fists. Also
 * what walking does to the world: everything comes nearer, and a side-step swings the nearer things across the view. */
static inline uint16_t rcp_of(uint16_t dist_q2) {uint16_t i=dist_q2>>5;return m6_rcp[i<200?i:200];}   /* 214 * 256 / distance, a distance in 1/4 unit */
static inline int16_t swung(int16_t ang,uint16_t dist,int16_t swing) {   /* the bearing after a side-step of `swing` (1/8 unit): swing / distance radians */
    int16_t d=(int16_t)(((uint16_t)abs16(swing)*rcp_of(dist))>>7);
    return wrapq(ang-(swing<0?-d:d));
}
static void fire_bolt(void) {
    Shot6 *s=shot_new();if(!s)return;
    int16_t side=A.gun_side?1:-1;A.gun_side^=1;
    int16_t ta=A.aim,tz=128;uint16_t td=900*4;
    if(A.lock!=255){Mech6 *m=&A.mech[A.lock];ta=A.aim+relq(m->ang);td=m->dist;tz=(int16_t)SCALE16[m->variant]*35;}
    int16_t start=A.aim+side*(188*16);   /* the shoulder guns: 44 units out at 50 ahead of the glass */
    int16_t steps=(td-200)/100;if(steps<1)steps=1;else if(steps>200)steps=200;
    *s=(Shot6){start,120,(ta-start)/steps,100,(tz-120)/steps,0,200,(uint8_t)(steps+1),0,1};   /* 25 units a step (1500 a second); gone on arrival (it would carry on across the middle to the other side) */
    audio_effect(1);
}
void m6_player(void) {
    uint8_t keys=pce_control.keys;
    bool aim=keys&KEY_SELECT;
    int16_t turn=0;
    if(!aim){if(keys&KEY_LEFT)turn-=96;if(keys&KEY_RIGHT)turn+=96;}   /* 1.7 radians a second: 6 dots a step */
    A.turn_v+=A.turn_v<turn?(turn-A.turn_v>6?6:turn-A.turn_v):(A.turn_v-turn>6?-6:turn-A.turn_v);
    A.aim=wrapq(A.aim+A.turn_v);
    A.cam+=A.turn_v;
    int16_t want=(keys&KEY_UP)?18:(keys&KEY_DOWN)?-12:0;
    A.speed+=A.speed<want?1:A.speed>want?-1:0;
    int16_t st=0;if(aim){if(keys&KEY_LEFT)st-=16;if(keys&KEY_RIGHT)st+=16;}
    A.strafe_v+=A.strafe_v<st?(st-A.strafe_v>2?2:st-A.strafe_v):(A.strafe_v-st>2?-2:st-A.strafe_v);
    A.lat+=A.strafe_v;
    /* the floor runs with the walking and the strafing: the texture's line (1/128 each: a line is 8 units) and, by depth, the shift of its four bands */
    A.along+=A.speed*2;if(A.along>=12288)A.along-=12288;else if(A.along<0)A.along+=12288;
    {static const int16_t PCOEF[4]={199,313,441,597};for(uint8_t b=0;b<4;++b)A.par[b]+=(A.strafe_v*PCOEF[b])>>8;}
    if(A.speed||A.strafe_v) {
        int16_t closing=A.speed>>1,swing=A.strafe_v;
        for(uint8_t k=0;k<3;++k) {
            Mech6 *m=&A.mech[k];if(m->st==S_OFF)continue;
            m->dist=(uint16_t)((int16_t)m->dist-closing);if(m->dist<240)m->dist=240;
            m->ang=swung(m->ang,m->dist,swing);
        }
        for(uint8_t k=0;k<16;++k) {
            Shot6 *s=&A.shot[k];if(!s->life||!s->enemy)continue;
            s->dist=(uint16_t)((int16_t)s->dist-closing);
            s->ang=swung(s->ang,s->dist,swing);
        }
        for(uint8_t k=0;k<14;++k) {
            Prop6 *p=&A.prop[k];
            p->dist=(uint16_t)((int16_t)p->dist-closing);
            if(p->dist<200||p->dist>7000)p->dist=closing>0?6400:240;   /* the field goes on: what has passed Ramrod comes round far ahead */
            p->ang=swung(p->ang,p->dist,swing);
        }
    }
    /* the guns: held fire, alternating shoulders; the heat builds up */
    if(A.gun_cd)--A.gun_cd;if(A.punch_cd)--A.punch_cd;if(A.hurt)--A.hurt;
    if(A.gun_idle<255)++A.gun_idle;
    if(!(keys&KEY_1))A.fire_hold=0;
    if(!(keys&KEY_2))A.punch_hold=0;
    if((keys&KEY_1)&&!A.fire_hold&&!A.overheated&&!A.gun_cd) {
        fire_bolt();A.gun_cd=8;A.heat+=9;A.gun_idle=0;
        if(A.heat>=250){A.heat=255;A.overheated=1;A.msg=1;A.msg_t=72;}
    }
    if(A.heat){uint8_t cool=A.gun_idle>10?3:1;A.heat=A.heat>cool?A.heat-cool:0;}   /* the guns cool a good deal faster than the PC's */
    if(A.overheated&&A.heat<120)A.overheated=0;
    /* the fists: alternate left / right, the hit lands as the arm reaches out */
    if((keys&KEY_2)&&!A.punch_hold&&!A.punch_cd&&A.punch_t<0){
        A.punch_t=0;A.punch_side^=1;A.punch_hit=0;A.punch_cd=30;A.punch_hold=1;if(A.speed<18)A.speed+=6;audio_effect(4);
    }
    if(A.punch_t>=0) {
        ++A.punch_t;
        if(!A.punch_hit&&A.punch_t>=11) {
            A.punch_hit=1;
            Mech6 *best=0;uint16_t bd=0xffff;
            for(uint8_t k=0;k<3;++k) {
                Mech6 *m=&A.mech[k];if(m->st==S_OFF||m->st==S_DYING)continue;
                uint16_t d=m->dist>>2;
                if(d<250+(34*SCALE16[m->variant]>>4)&&abs16(relq(m->ang))<(int16_t)((((uint16_t)(70+(d>>2))*(rcp_of(m->dist)>>3))>>5)*16)&&d<bd){bd=d;best=m;}
            }
            if(best) {
                bool counter=best->st==S_WINDUP;
                mech_damage(best,counter?9:6,true);
                if(counter&&best->st==S_STAGGER){A.msg=2;A.msg_t=48;}
                audio_effect(4);if(A.shake<12)A.shake=12;
                burst(best->ang,best->dist-160,(int16_t)SCALE16[best->variant]*35,2,9);
            }
            for(uint8_t k=0;k<16;++k) {   /* a fist swats an incoming plasma ball out of the air */
                Shot6 *s=&A.shot[k];if(!s->life||!s->enemy)continue;
                if(s->dist<800&&abs16(relq(s->ang))<640){s->life=0;burst(s->ang,s->dist,s->z,0,30);A.msg=3;A.msg_t=42;}
            }
        }
        if(A.punch_t>34)A.punch_t=-1;
    }
}

/* ---- the HUD (the picture's m6_d.c calls it): armour and gun heat, the wave, what is left, the spares, the radar, the arrows for mechs out of view, the banners ----
 * The fixed HUD is not in the sprite cache (which it would crowd with a page for every piece): every piece is one 16x16 pattern in a slot of its own in $7800-$7dff (the
 * background characters' clip area, unused here), written when its picture changes (a bar's length, a digit); the palette is 31, the cache's shared one. */
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page,sat_count,sprite_line_ok,sprite_line_lo,sprite_line_hi;
void sprite_lines_reserve(void);
extern const PceScene *video_scene_ptr;
#define HUD_WORD 0x7800
#define RETICLE_Y 121
static uint16_t hud_key[18]={0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff};
static int16_t hud_dx[18],hud_dy[18],hud_base;
enum {SL_AR,SL_ARBAR,SL_GN,SL_GNBAR,SL_WAVE,SL_LEFT,SL_LIVES,SL_RETICLE,SL_LOCK,SL_RADAR,SL_DOT=SL_RADAR+4};   /* SL_DOT: five colours */
static void hud_slot(uint8_t slot,uint16_t offset,int16_t x,int16_t y) {
    uint16_t id=hud_base+offset;
    if(hud_key[slot]!=id) {
        uint8_t e[16],pc[6];
        arcade_read(2,video_scene_ptr->sprites+(uint32_t)id*16,e,16);
        arcade_read(2,e[4]|(uint32_t)e[5]<<8|(uint32_t)e[6]<<16|(uint32_t)e[7]<<24,pc,6);
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(e[0]|(uint32_t)e[1]<<8|(uint32_t)e[2]<<16|(uint32_t)e[3]<<24,HUD_WORD+slot*64,128);
        hud_dx[slot]=(int16_t)(pc[0]|(uint16_t)pc[1]<<8);hud_dy[slot]=(int16_t)(pc[2]|(uint16_t)pc[3]<<8);hud_key[slot]=id;
    }
    int16_t px=x+hud_dx[slot],py=y+hud_dy[slot];
    if(px<=-16||px>=256||py<=-16||py>=224||sat_count>=64)return;
    sprite_line_lo=py<0?0:py;sprite_line_hi=py+16>224?224:py+16;
    sprite_lines_reserve();if(!sprite_line_ok)return;
    sat[sat_page][sat_count++]=(vdc_sprite_t){py+64,px+32,(HUD_WORD>>5)+slot*2,VDC_SPRITE_FG|15};
}
static void hud_put(uint16_t offset,int16_t x,int16_t y,bool flip) {video_sprite_optional(hud_base+offset,x,y,flip,16);}   /* (banners and messages: through the cache) */
static void bar(uint8_t slot,int16_t x,int16_t y,uint8_t px,uint8_t color) {   /* up to 16 px: one piece */
    if(px>16)px=16;
    if(px)hud_slot(slot,PCE_H6_BAR_GREEN_1+(uint16_t)color*16-1+px,x,y);
}
static const int8_t SIN64[64]={0,12,25,37,49,60,71,81,90,98,106,112,117,122,125,126,127,126,125,122,117,112,106,98,90,81,71,60,49,37,25,12,0,-12,-25,-37,-49,-60,-71,-81,-90,-98,-106,-112,-117,-122,-125,-126,-127,-126,-125,-122,-117,-112,-106,-98,-90,-81,-71,-60,-49,-37,-25,-12};
/* The radar (ramrod.c render_monitors) in the top right corner: forward is up, Ramrod at the bottom centre, a dot for every mech (green, red for the heavy, gold for
 * the Commander, white as it is about to fire or swing) and a pink one for each plasma ball coming (the nearest four); one pixel is 64 units (the panel is 32x32 and Ramrod stands at its foot). The dots are admitted
 * before the panel, which is therefore behind them. */
#define RADAR_X 220
#define RADAR_Y 4
static void radar_dot(uint8_t colour,int16_t ang,uint16_t dist_q2) {
    int16_t rel=ang-a6.aim;if(rel<0)rel+=ARC;
    uint8_t i=(uint8_t)((uint16_t)rel/336)&63;
    int16_t d=dist_q2>>6;   /* units / 16 */
    int16_t f=(int16_t)(d*SIN64[(i+16)&63])>>9,l=(int16_t)(d*SIN64[i])>>9;
    int16_t x=RADAR_X+16+l-1,y=RADAR_Y+24-f-1;
    if(x<RADAR_X+2)x=RADAR_X+2;else if(x>RADAR_X+27)x=RADAR_X+27;
    if(y<RADAR_Y+2)y=RADAR_Y+2;else if(y>RADAR_Y+27)y=RADAR_Y+27;
    hud_slot(SL_DOT+colour,PCE_H6_RDOT_GREEN+colour,x,y);
}
static void radar(void) {
    for(uint8_t k=0;k<3;++k) {
        Mech6 *m=&a6.mech[k];if(m->st==S_OFF||m->st==S_DYING)continue;
        bool blink=(m->st==S_AIM||m->st==S_WINDUP||m->st==S_CHARGE)&&(pce_ticks&4);
        radar_dot(blink?3:m->variant==2?2:m->variant==1?1:0,m->ang,m->dist);
    }
    uint8_t n=0;
    for(uint8_t k=0;k<16&&n<2;++k) {
        Shot6 *s=&a6.shot[k];if(!s->life||!s->enemy)continue;
        radar_dot(4,s->ang,s->dist);++n;
    }
    hud_slot(SL_RADAR,PCE_H6_RADAR_0,RADAR_X,RADAR_Y);hud_slot(SL_RADAR+1,PCE_H6_RADAR_1,RADAR_X+16,RADAR_Y);
    hud_slot(SL_RADAR+2,PCE_H6_RADAR_2,RADAR_X,RADAR_Y+16);hud_slot(SL_RADAR+3,PCE_H6_RADAR_3,RADAR_X+16,RADAR_Y+16);
}
void m6_hud(void) {
    hud_base=pce_hud_base[5];
    /* the reticle (Battlezone's red brackets), lit while a mech is locked, and the lock's armour bar above its head (a6.lock_*: the picture's) */
    hud_slot(SL_RETICLE,PCE_H6_RETICLE0+(A.lock!=255),128,RETICLE_Y);
    if(A.lock!=255&&A.lock_hp)hud_slot(SL_LOCK,PCE_H6_BAR_RED_1-1+(A.lock_hp>16?16:A.lock_hp),A.lock_x,A.lock_y);
    uint16_t armor=pce_metrics.hp>100?100:pce_metrics.hp;
    hud_slot(SL_AR,PCE_H6_LAB_AR,4,4);
    bar(SL_ARBAR,22,6,(uint8_t)((armor*41)>>8),armor>50?0:armor>25?1:2);
    hud_slot(SL_GN,A.overheated?PCE_H6_LAB_HT:PCE_H6_LAB_GN,4,14);
    bar(SL_GNBAR,22,16,A.heat>>4,A.overheated?2:3);
    hud_slot(SL_WAVE,PCE_H6_LAB_W1+(pce_campaign.wave<2?pce_campaign.wave:2),4,24);
    hud_slot(SL_LEFT,PCE_H6_DIGIT_PINK_0+A.left,22,24);
    hud_slot(SL_LIVES,PCE_H6_LAB_X0+(pce_campaign.lives>9?9:pce_campaign.lives),32,24);
    radar();
    /* mechs outside the view: an arrow on that side, blinking when one is about to fire or swing */
    for(uint8_t k=0;k<3;++k) {
        uint8_t t=A.threat[k];if(!(t&3))continue;
        bool danger=t&4;if(danger&&(pce_ticks&8))continue;
        hud_put(danger?PCE_H6_CHEVRON_DANGER:PCE_H6_CHEVRON,(t&3)==1?6:242,100+k*8,(t&3)==1);
    }
}
/* the banners and messages: drawn after the world (the least important, the first to go when the sprite table is full) and moved to the front by the picture */
void m6_msgs(void) {
    hud_base=pce_hud_base[5];
    if(A.banner&&pce_campaign.wave<3) {
        uint8_t w=pce_campaign.wave;
        if(A.banner>40||(A.banner&8))hud_put(PCE_H6_WAVE1+w,128-PCE_H6_WAVE1_W/2,44,false);
        if(w==2&&A.banner>60)hud_put(PCE_H6_WARNING,128-PCE_H6_WARNING_W/2,60,false);
    }
    if(A.msg_t)hud_put(A.msg==1?PCE_H6_HOT:A.msg==2?PCE_H6_COUNTER:PCE_H6_PARRY,128-(A.msg==1?12:A.msg==2?PCE_H6_COUNTER_W:PCE_H6_PARRY_W)/2,76,false);
}
