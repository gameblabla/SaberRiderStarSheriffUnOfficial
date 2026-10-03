#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
#include <string.h>
#define SPACE_CODE __attribute__((noinline,section(".ram_bank115.text")))
typedef struct __attribute__((packed)) {uint16_t time;uint8_t kind,n;int16_t y;uint16_t gap;uint8_t pattern,drop;} Event;
typedef struct {int16_t x,y;uint8_t kind,hp,pattern,drop;uint16_t clock;uint8_t charge;} Foe;
typedef struct {int16_t x,y;int8_t vx,vy;uint8_t on,enemy;} Bolt;
static Foe foes[8];
static Bolt bolts[12];
static Event next_event;
static uint16_t flight_clock,spawn_clock,beam_clock,power_timer;
static uint8_t event_index,event_count,pending,gun_clock,hurt,power,bombs;
static int16_t ship_x,ship_y,pickup_x,pickup_y,space_boss_y;
static uint8_t pickup,port_hp[7];
static const uint8_t port_y[7] __attribute__((section(".ram_bank115.rodata")))={25,24,24,23,36,32,41};
SPACE_CODE static int16_t dist(int16_t a,int16_t b) {int16_t n=a-b;return n<0?-n:n;}
SPACE_CODE static void event_load(void) {
    if(event_index<event_count)arcade_read(2,pce_scenes[6].track+1+(uint16_t)event_index*10,&next_event,10);
}
SPACE_CODE void space_start(void) {
    memset(foes,0,sizeof foes);memset(bolts,0,sizeof bolts);
    flight_clock=spawn_clock=beam_clock=power_timer=0;event_index=pending=gun_clock=hurt=pickup=0;
    pce_campaign.power_cd=pce_campaign.timer=0;
    power=1;bombs=3;ship_x=48;ship_y=112;space_boss_y=80;
    arcade_read(2,pce_scenes[6].track,&event_count,1);event_load();
    for(uint8_t k=0;k<7;++k)port_hp[k]=20;
    pce_campaign.boss_kind=0;pce_campaign.boss_hp=1200;pce_campaign.boss_round=0;pce_metrics.hp=4;
}
SPACE_CODE static void bolt(int16_t x,int16_t y,int8_t vx,int8_t vy,bool enemy) {
    for(uint8_t k=0;k<12;++k)if(!bolts[k].on){bolts[k]=(Bolt){x,y,vx,vy,1,enemy};return;}
}
SPACE_CODE static void foe_kill(Foe *f) {
    f->kind=0;++pce_campaign.score;audio_effect(4);
    if(f->drop){pickup=f->drop;pickup_x=f->x;pickup_y=f->y;}
}
SPACE_CODE static void spawn(void) {
    for(uint8_t k=0;k<8;++k)if(!foes[k].kind) {
        uint8_t n=next_event.n-pending;
        foes[k]=(Foe){272,next_event.y+(next_event.kind==4?0:n*next_event.gap),
            next_event.kind,next_event.kind==5?22:3,next_event.pattern,next_event.drop,0,0};
        if(foes[k].y>208)foes[k].y=208;
        spawn_clock=next_event.kind==4?next_event.gap:20;
        if(!--pending){++event_index;event_load();}return;
    }
}
SPACE_CODE static void space_tick(void) {
    uint8_t keys=pce_control.keys;
    if(power_timer) {
        pce_campaign.timer=--power_timer;
        if(!power_timer) {
            for(uint8_t k=0;k<8;++k)if(foes[k].kind)foe_kill(&foes[k]);
            for(uint8_t k=0;k<12;++k)if(bolts[k].enemy)bolts[k].on=0;
            if(pce_campaign.boss_kind) {
                pce_campaign.boss_hp=pce_campaign.boss_hp>84?pce_campaign.boss_hp-84:0;
                for(uint8_t k=0;k<7;++k)if(k<5||pce_campaign.boss_round)port_hp[k]=0;
            }
            hurt=150;pce_campaign.power_cd=720;audio_effect(4);video_restore();
        }
        return;
    }
    if(pce_campaign.power_cd)--pce_campaign.power_cd;
    if((pce_control.pressed&KEY_SELECT)&&(keys&KEY_1)&&pce_campaign.powers&&!pce_campaign.power_cd) {
        --pce_campaign.powers;power_timer=114;pce_campaign.timer=114;return;
    }
    ++flight_clock;if(gun_clock)--gun_clock;if(hurt)--hurt;
    uint8_t velocity=keys&KEY_SELECT?1:2;
    if(keys&KEY_LEFT)ship_x-=velocity;if(keys&KEY_RIGHT)ship_x+=velocity;
    if(keys&KEY_UP)ship_y-=velocity;if(keys&KEY_DOWN)ship_y+=velocity;
    if(ship_x<16)ship_x=16;if(ship_x>240)ship_x=240;
    if(ship_y<16)ship_y=16;if(ship_y>208)ship_y=208;
    if(pce_campaign.boss_kind&&ship_x>112)ship_x=112;
    if((keys&KEY_1)&&!gun_clock) {bolt(ship_x+22,ship_y,6,0,false);gun_clock=8;audio_effect(1);}
    if((pce_control.pressed&KEY_2)&&bombs) {
        --bombs;audio_effect(4);
        for(uint8_t k=0;k<8;++k)if(foes[k].kind) {
            Foe *f=&foes[k];f->hp=f->hp>14?f->hp-14:0;
            if(f->kind<=3||!f->hp)foe_kill(f);
        }
        for(uint8_t k=0;k<12;++k)if(bolts[k].enemy)bolts[k].on=0;
        if(pce_campaign.boss_kind){pce_campaign.boss_hp=pce_campaign.boss_hp>28?pce_campaign.boss_hp-28:0;
            for(uint8_t k=0;k<7;++k)if(k<5||pce_campaign.boss_round)port_hp[k]=port_hp[k]>10?port_hp[k]-10:0;
        }
        hurt=60;
    }
    if(pending){if(spawn_clock)--spawn_clock;else spawn();}
    else if(event_index<event_count&&flight_clock>=next_event.time) {
        pending=next_event.n;spawn();
        /* Keep the current recipe until all its entities have spawned. */
    }
    for(uint8_t k=0;k<8;++k) {
        Foe *f=&foes[k];if(!f->kind)continue;++f->clock;
        f->x-=f->kind==4?2:1;
        if(f->pattern==1){if((f->clock&63)<32)++f->y;else --f->y;}
        if(f->pattern==2&&f->x>80)f->y+=f->y<ship_y?1:-1;
        if(f->kind>=4&&!(f->clock%90))bolt(f->x-12,f->y,-3,ship_y<f->y?-1:1,true);
        if(f->kind==2&&(f->charge||(f->clock>=144&&ship_x<f->x-30&&dist(ship_y,f->y)<34&&f->x<244))) {
            if(++f->charge>=39){bolt(f->x-22,f->y,-4,0,true);f->charge=0;f->clock=0;audio_effect(1);}
        }
        if(!hurt&&dist(ship_x,f->x)<(f->kind<=3?24:18)&&dist(ship_y,f->y)<(f->kind<=3?26:10)) {
            hurt=90;campaign_hurt();
        }
        if(f->x< -48)f->kind=0;
    }
    if(flight_clock>=6360&&!pce_campaign.boss_kind) {
        pce_campaign.boss_kind=5;beam_clock=0;audio_music(11);
        memset(foes,0,sizeof foes);
    }
    if(pce_campaign.boss_kind) {
        ++beam_clock;
        pce_campaign.boss_round=pce_campaign.boss_hp<396?2:pce_campaign.boss_hp<792?1:0;
        if(pce_campaign.boss_round==2&&beam_clock%300>=180)space_boss_y+=ship_y<space_boss_y+28?-1:1;
        if(space_boss_y<30)space_boss_y=30;if(space_boss_y>145)space_boss_y=145;
        for(uint8_t k=0;k<7;++k)if(port_hp[k]&&(k<5||pce_campaign.boss_round)&&beam_clock%(90+k*12)==0)
            bolt(135,space_boss_y+port_y[k],-3,ship_y<space_boss_y+port_y[k]?-1:1,true);
        if(beam_clock%300>220&&!hurt&&dist(ship_y,space_boss_y+28)<8){hurt=90;campaign_hurt();}
    }
    for(uint8_t k=0;k<12;++k) {
        Bolt *b=&bolts[k];if(!b->on)continue;
        b->x+=b->vx;b->y+=b->vy;
        if(b->x<0||b->x>256||b->y<0||b->y>224){b->on=0;continue;}
        if(b->enemy) {
            if(!hurt&&dist(ship_x,b->x)<6&&dist(ship_y,b->y)<6){b->on=0;hurt=90;campaign_hurt();}
        } else {
            for(uint8_t j=0;j<8&&b->on;++j) {
                Foe *f=&foes[j];if(!f->kind||dist(b->x,f->x)>24||dist(b->y,f->y)>16)continue;
                b->on=0;
                /* Armoured mines deflect ordinary shots, as in space.c. */
                if(f->kind>=4){f->hp=f->hp>power?f->hp-power:0;if(!f->hp)foe_kill(f);}
            }
            if(b->on&&pce_campaign.boss_kind&&b->x>=128&&b->y>=space_boss_y&&b->y<space_boss_y+64) {
                b->on=0;
                uint16_t damage=power;
                for(uint8_t j=0;j<7;++j)if(port_hp[j]&&(j<5||pce_campaign.boss_round)&&dist(b->y,space_boss_y+port_y[j])<4) {
                    port_hp[j]=port_hp[j]>power?port_hp[j]-power:0;if(!port_hp[j])damage+=30;break;
                }
                pce_campaign.boss_hp=pce_campaign.boss_hp>damage?pce_campaign.boss_hp-damage:0;
            }
        }
    }
    if(pickup) {
        --pickup_x;
        if(dist(pickup_x,ship_x)<18&&dist(pickup_y,ship_y)<16) {
            if(pickup==1){if(power<3)++power;else if(bombs<5)++bombs;}
            else if(pickup==2){if(pce_metrics.hp<4)++pce_metrics.hp;}
            else if(bombs<5)++bombs;
            pickup=0;audio_effect(3);
        }
        if(pickup_x<0)pickup=0;
    }
    if(pce_campaign.boss_kind&&!pce_campaign.boss_hp){pce_campaign.result=1;pce_campaign.story=0;pce_campaign.event=1;}
}
SPACE_CODE void space_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i) {
        space_tick();pce_control.pressed=0;
    }
    video_background((flight_clock>>3)&255);video_sat_begin();
    if(!hurt||(flight_clock&4))video_sprite(3,ship_x,ship_y,false,16);
    if(pce_campaign.boss_kind)video_sprite(6,128,space_boss_y+32,false,16);
    for(uint8_t k=0;k<12;++k)if(bolts[k].on)
        if(!video_sprite_optional(bolts[k].enemy?1:0,bolts[k].x,bolts[k].y,false,16))bolts[k].on=0;
    for(uint8_t k=0;k<8;++k)if(foes[k].kind) {
        Foe *f=&foes[k];
        if(!video_sprite_optional(f->kind<=3?8:f->kind==5?5:4,f->x,f->y,true,16)){f->kind=0;continue;}
        if(f->charge>20&&(flight_clock&4))video_sprite_optional(1,f->x-20,f->y,false,16);
    }
    if(pickup)video_sprite_optional(8+pickup,pickup_x,pickup_y,false,16);
    if(pce_campaign.boss_kind&&beam_clock%300>220)
        for(uint8_t x=16;x<128;x+=16)video_sprite_optional(1,x,space_boss_y+28,false,16);
    video_sat_end();video_text(1,0,"RAMROD HP");video_number(11,0,pce_metrics.hp);
    video_text(18,0,"B");video_number(20,0,bombs);
    if(pce_campaign.boss_kind){video_text(1,1,"CRUISER");video_number(10,1,pce_campaign.boss_hp);}
    video_text(18,1,"P");video_number(20,1,pce_campaign.powers);
    if(power_timer)video_text(4,12,"HERO POWER - RAMROD BLAST");
    pce_metrics.player_x=ship_x;pce_metrics.player_y=ship_y;
}
