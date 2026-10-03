#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
#include "save_pce.h"
#include "floor_pce.h"
typedef struct __attribute__((packed)) {uint16_t x,y;uint8_t heading;} TrackPoint;
static TrackPoint track[256] PCE_STAGE;
static uint16_t race_progress,rival_progress[7],race_time,gap;
static uint8_t rival_laps[7],speed,hurt,fire_cd,race_phase,turbo;
static int16_t lateral;
static const int8_t race_sine[32] __attribute__((section(".ram_bank111.rodata")))={0,25,49,71,90,106,117,125,127,125,117,106,90,71,49,25,0,-25,-49,-71,-90,-106,-117,-125,-127,-125,-117,-106,-90,-71,-49,-25};
PCE_MISSION void race_start(void) {
    arcade_read(1,pce_scenes[1].track,track,sizeof track);
    race_progress=0;race_time=0;gap=1200;
    lateral=0;speed=hurt=fire_cd=0;turbo=120;
    race_phase=pce_control.phase?2:0;
    for(uint8_t k=0;k<7;++k){rival_progress[k]=(7-k)*240;rival_laps[k]=0;}
    pce_campaign.lap=1;pce_campaign.rank=8;pce_campaign.boss_kind=0;pce_campaign.boss_hp=80;
    pce_metrics.hp=8;pce_campaign.timer=180;
}
PCE_MISSION static void race_tick(void) {
    uint8_t keys=pce_control.keys;
    if(hurt)--hurt;if(fire_cd)--fire_cd;
    if(pce_campaign.timer){--pce_campaign.timer;return;}
    ++race_time;
    uint8_t maximum=112;
    if((keys&KEY_2)&&turbo){maximum=160;--turbo;}else if(turbo<120&&!(race_time&3))++turbo;
    if(lateral>104||lateral< -104)maximum=48;
    if(keys&KEY_UP){if(speed<maximum)++speed;else if(speed>maximum)--speed;}
    else if(speed)--speed;
    if(keys&KEY_DOWN){speed=speed>2?speed-2:0;}
    if(keys&KEY_LEFT)lateral-=2;if(keys&KEY_RIGHT)lateral+=2;
    if(lateral< -160)lateral=-160;if(lateral>160)lateral=160;
    if(race_phase<2) {
        uint16_t old=race_progress;race_progress+=speed;
        if(race_progress<old)++pce_campaign.lap;
        uint8_t rank=1;
        for(uint8_t k=0;k<7;++k) {
            uint16_t prior=rival_progress[k];rival_progress[k]+=86+k*4;
            if(rival_progress[k]<prior)++rival_laps[k];
            if(rival_laps[k]+1>pce_campaign.lap||(rival_laps[k]+1==pce_campaign.lap&&rival_progress[k]>race_progress))++rank;
            if(!hurt&&((int16_t)(rival_progress[k]-race_progress))<180&&((int16_t)(rival_progress[k]-race_progress))> -180&&
                lateral>(k&1?28:-68)&&lateral<(k&1?68:-28)) {hurt=45;speed>>=1;campaign_hurt();}
        }
        pce_campaign.rank=rank;
        if(pce_campaign.lap>=4) {
            if(rank>3){campaign_hurt();pce_campaign.result=2;}
            else {race_phase=2;lateral=0;race_progress=0;gap=1200;pce_campaign.story=1;pce_campaign.event=1;
                save_store(2,pce_control.hero,1);audio_music(9);}
        }
    } else {
        race_progress+=speed;
        if(speed>72)gap=gap>(speed-72)/8?gap-(speed-72)/8:0;
        else if(!(race_time&3)&&gap<4200)++gap;
        if(race_phase==2&&gap<=260){race_phase=3;pce_campaign.boss_kind=4;pce_campaign.story=2;pce_campaign.event=1;}
        if(race_phase==3&&(keys&KEY_1)&&!fire_cd) {
            fire_cd=8;audio_effect(1);
            if(lateral> -48&&lateral<48&&pce_campaign.boss_hp)--pce_campaign.boss_hp;
        }
        if(race_phase==3&&!hurt&&(keys&KEY_2)&&speed>=112&&lateral> -48&&lateral<48) {
            pce_campaign.boss_hp=pce_campaign.boss_hp>4?pce_campaign.boss_hp-4:0;hurt=60;campaign_hurt();
        }
        if(race_phase==3&&!(race_time%90)&&!hurt&&lateral> -40&&lateral<40){hurt=60;campaign_hurt();}
        if(!pce_campaign.boss_hp){pce_campaign.result=1;pce_campaign.story=3;pce_campaign.event=1;}
    }
}
PCE_MISSION void race_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i)race_tick();
    TrackPoint *t=&track[race_progress>>8];
    uint8_t heading=t->heading;
    uint16_t x=t->x-(lateral*race_sine[heading]>>7),y=t->y+(lateral*race_sine[(heading+8)&31]>>7);
    if(race_phase>=2){x=4096+lateral;y=4096-(race_progress>>3);heading=24;}
    pce_control.x=x;pce_control.y=y;pce_control.heading=heading;pce_control.phase=race_phase>=2;
    overlay_call(0x6d,floor_draw);
    video_sat_begin();video_sprite(5,256,215,false,16);
    if(race_phase<2) {
        for(uint8_t k=0,shown=0;k<7&&shown<3;++k) {
            int16_t d=rival_progress[k]-race_progress;
            if(d>0&&d<6000){video_sprite_optional(k<3?7:13,256+(k&1?70:-70)-lateral,200-(d>>8),false,16);++shown;}
        }
    } else {
        video_sprite(race_phase==3?10:9,256-lateral,190-(gap>>5),false,16);
        if(race_phase==3&&!(race_time&32))video_sprite_optional(21,256-lateral,203,false,16);
        if(pce_control.keys&KEY_1)video_sprite_optional(0,256,185,false,16);
    }
    video_sat_end();overlay_call(0x6d,floor_present);
    video_text(1,0,race_phase<2?"GRAND PRIX  LAP":"HORNET PURSUIT");
    video_number(16,0,race_phase<2?pce_campaign.lap:gap);
    video_text(1,1,"HP");video_number(4,1,pce_metrics.hp);
    video_text(12,1,race_phase<2?"RANK":"BOSS");video_number(18,1,race_phase<2?pce_campaign.rank:pce_campaign.boss_hp);
    if(pce_campaign.timer)video_text(1,2,"READY");else video_text(1,2,"UP GAS II TURBO I FIRE");
    pce_metrics.phase=race_phase>=2;pce_metrics.player_x=x;pce_metrics.player_y=y;
}
