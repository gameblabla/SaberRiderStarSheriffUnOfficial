#pragma once
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "audio_pcm.h"
#include "assets.h"
#include "m6_state.h"
#define A a6
#define M6U __attribute__((unused))
/* Helpers each image carries its own copy of (static: the ones an image does not use take no room). */
static const M6U uint8_t HP[3]={16,34,90},SPEED[3]={5,6,5},VOLLEY[3]={1,3,4},SHOT_DMG[3]={8,6,8},PUNCH_DMG[3]={14,16,22};
static const M6U uint8_t SCALE16[3]={16,17,21};
static inline int16_t abs16(int16_t v) {return v<0?-v:v;}
static inline int16_t wrapq(int16_t a) {if(a>=ARC)a-=ARC;else if(a<0)a+=ARC;return a;}
/* a bearing relative to where Ramrod looks, in 1/16 dot, within half a turn */
static inline int16_t relq(int16_t ang) {
    int16_t r=ang-A.aim;
    if(r>ARC/2)r-=ARC;else if(r<-ARC/2)r+=ARC;
    return r;
}
static inline uint8_t rnd(void) {
    uint16_t r=A.rng;uint8_t carry=r&1;r>>=1;if(carry)r^=0xB400;A.rng=r;
    return (uint8_t)(r^(r>>8));
}
M6U static void burst(int16_t ang,uint16_t dist,int16_t z,uint8_t size,uint8_t dur) {   /* an explosion (dur 30), or a hit's flash (the first frames, dur 9) */
    for(uint8_t k=0;k<10;++k)if(!A.fx[k].dur){A.fx[k]=(Fx6){ang,z,dist,0,dur,size};return;}
}
M6U static Shot6 *shot_new(void) {
    for(uint8_t k=0;k<16;++k)if(!A.shot[k].life)return &A.shot[k];
    return 0;
}
M6U static void hurt_player(uint8_t dmg,uint8_t shake) {
    if(A.hurt)return;
    uint16_t d=(dmg*(pce_options.difficulty==0?3:pce_options.difficulty==1?4:5)+7)>>3;   /* (the PC's 100 armour is worth 200 here: the arena is the hardest stage on this hardware) */
    A.hurt=24;if(A.shake<shake)A.shake=shake;A.flash_red=22;
    uint16_t hp=pce_metrics.hp;
    pce_metrics.hp=hp>d?hp-d+1:1;   /* campaign_hurt takes one off, and the armour is gone at zero */
    campaign_hurt();
}
M6U static void mech_damage(Mech6 *m,uint8_t dmg,bool punch) {
    if(m->st==S_DYING||m->st==S_OFF)return;
    m->flash=7;
    if(m->hp<=dmg) {   /* it goes down: burning, sinking into its own blast */
        m->st=S_DYING;m->dying=0;m->expl_t=0;m->hp=0;
        ++A.killed;++pce_campaign.score;audio_effect(16);
        if(A.lock<3&&&A.mech[A.lock]==m)A.lock=255;
        return;
    }
    m->hp-=dmg;
    if(punch){uint8_t t=m->st==S_WINDUP?60:45;m->kick=24;m->st=S_STAGGER;m->st_t=t;}   /* a fist rocks it back; one caught winding up loses its punch */
}
