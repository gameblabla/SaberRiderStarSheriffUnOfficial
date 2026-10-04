#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "assets.h"
#include "hud_pce.h"
#define MECH_CODE __attribute__((noinline,section(".ram_bank114.text")))
Mech mechs[3];
uint8_t visible[3];
uint16_t wave_clock;
uint8_t spawned,killed,aim,heat,gun_cd,punch_cd,overheated;
static uint8_t hurt;
/* Source WAVES in ramrod.c: four, six, then eight mechs. */
static const uint8_t variants[3][8] __attribute__((section(".ram_bank114.rodata")))={{0,0,0,0},{0,0,1,0,1,0},{2,0,0,1,0,1,0,1}};
static const uint16_t delays[3][8] __attribute__((section(".ram_bank114.rodata")))={{0,120,360,600},{0,30,240,420,600,720},{0,18,36,360,480,780,900,1080}};
static const int8_t angles[3][8] __attribute__((section(".ram_bank114.rodata")))={{0,25,-28,11},{-74,74,6,110,-100,-23},{0,-34,34,100,-100,110,91,-69}};
MECH_CODE void mech_start(void) {
    for(uint8_t k=0;k<3;++k){mechs[k].on=0;visible[k]=0;}
    spawned=killed=heat=gun_cd=punch_cd=hurt=overheated=0;aim=128;wave_clock=0;
    pce_campaign.wave=pce_control.phase<3?pce_control.phase:0;pce_campaign.boss_hp=0;pce_campaign.boss_kind=0;pce_metrics.hp=100;
}
MECH_CODE static void hurt_mech(uint8_t damage) {
    if(hurt)return;
    hurt=30;
    pce_metrics.hp=pce_metrics.hp>damage?pce_metrics.hp-damage+1:1;
    campaign_hurt();
}
MECH_CODE static void mech_tick(void) {
    uint8_t keys=pce_control.keys,wave=pce_campaign.wave;
    ++wave_clock;
    if(gun_cd)--gun_cd;if(punch_cd)--punch_cd;if(hurt)--hurt;
    if(heat&&!(wave_clock&1))--heat;if(heat<30)overheated=0;
    if(keys&KEY_LEFT&&aim>24)aim-=2;if(keys&KEY_RIGHT&&aim<232)aim+=2;
    uint8_t total=wave==0?4:wave==1?6:8;
    if(spawned<total&&wave_clock>=delays[wave][spawned]) {
        for(uint8_t k=0;k<(wave?3:2);++k)if(!mechs[k].on) {
            uint8_t v=variants[wave][spawned];
            mechs[k]=(Mech){128+angles[wave][spawned],900,v==2?90:v==1?34:16,0,v,1};
            ++spawned;break;
        }
    }
    int8_t target=-1;uint16_t nearest=2000;
    for(uint8_t k=0;k<3;++k) {
        Mech *m=&mechs[k];if(!m->on)continue;++m->clock;
        int16_t dx=m->x-aim;if(dx<0)dx=-dx;
        if(dx<32&&m->distance<nearest){target=k;nearest=m->distance;}
        uint16_t closing=keys&KEY_UP?4:keys&KEY_DOWN?0:2;
        if(m->distance>120)m->distance-=closing;
        else if(keys&KEY_DOWN)m->distance+=2;
        if(visible[k]&&!(m->clock%120)&&dx<24)hurt_mech(m->variant==2?8:6);
        if(visible[k]&&m->distance<180&&!(m->clock%60)&&dx<32)hurt_mech(m->variant==2?22:14);
        if(m->clock%120>78&&m->clock%120<115)m->x+=m->x<128?1:-1;
    }
    uint8_t damage=0;
    if((keys&KEY_1)&&!gun_cd&&!overheated){gun_cd=8;heat+=5;if(heat>=100)overheated=1;damage=1;audio_effect(1);}
    if((keys&KEY_2)&&!punch_cd){punch_cd=45;if(nearest<280)damage=6;audio_effect(3);}
    if(damage&&target>=0) {
        Mech *m=&mechs[(uint8_t)target];
        m->hp=m->hp>damage?m->hp-damage:0;
        if(!m->hp){m->on=0;++killed;++pce_campaign.score;audio_effect(4);}
    }
    pce_campaign.boss_hp=target<0?0:mechs[(uint8_t)target].hp;
    if(killed==total) {
        if(wave==2){pce_campaign.result=1;pce_campaign.story=3;pce_campaign.event=1;}
        else {
            pce_campaign.story=wave+1;pce_campaign.event=1;++pce_campaign.wave;
            spawned=killed=0;wave_clock=0;pce_metrics.hp=100;heat=0;
        }
    }
}
MECH_CODE void mech_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i)mech_tick();
    overlay_call(0x7c,mech_draw);
}
