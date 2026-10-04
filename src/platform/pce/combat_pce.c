#include "campaign_pce.h"
#include "play_internal.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "loader_pce.h"
static uint8_t dialogs_done,ndialog,ndeath;
static uint16_t arena_time;
extern int16_t boss_x,boss_y;extern uint8_t boss_phase,boss_flash,boss_max;extern uint16_t boss_time;
void boss_start(void),boss_tick(void),boss_draw(void);
typedef struct { int16_t zone[4],focus; uint16_t before,after,voice; } DialogZone;
static DialogZone dialog_zones[4];
static int16_t death_zones[8][6];
/* Level dialogue with a camera focus (the outrider who spots the heroes): the hero comes to a stop, the camera pans
 * to the focus point 4 px a step, holds while the scene plays, the text runs, the scene plays on, and the camera
 * returns to the hero. Phases: 1 stop, 2 pan out, 3 hold, 4 text, 5 hold, 6 pan back. The world is frozen while
 * the camera pans (as in the main game). */
uint8_t cut_phase;
static uint8_t cut_k;
static uint16_t cut_wait,cut_target;
static inline __attribute__((always_inline)) int16_t distance(int16_t a,int16_t b) { int16_t n=a-b;return n<0?-n:n; }
PCE_COMBAT static bool zone(const int16_t *z) {
    return distance(player.x+4,z[0])<=z[2]+8&&distance(player.y+9,z[1])<=z[3]+23;
}
PCE_COMBAT static void bullet(int16_t x,int16_t y,int16_t vx,int16_t vy) {
    for(uint8_t k=0;k<NSHOTS;++k)if(!shots[k].active){shots[k]=(Shot){x,y,vx,vy,1,1,0,0,0};break;}
}
PCE_COMBAT void combat_start(void) {
    boss_phase=boss_flash=dialogs_done=ndialog=ndeath=cut_phase=0;boss_time=arena_time=0;
    pce_campaign.boss_kind=pce_campaign.boss_round=0;pce_campaign.boss_hp=0;
    pce_campaign.result=pce_campaign.event=0;pce_campaign.boost=pce_campaign.power_cd=0;
    uint8_t count[3];arcade_read(2,play_scene->rules,count,3);
    ndialog=count[0]>4?4:count[0];ndeath=count[1]>8?8:count[1];
    arcade_read(2,play_scene->rules+3,dialog_zones,(uint16_t)ndialog*sizeof(DialogZone));
    arcade_read(2,play_scene->rules+3+(uint16_t)ndialog*sizeof(DialogZone),death_zones,(uint16_t)ndeath*12);
}
PCE_COMBAT static void boss_begin(uint8_t kind) {
    camera=play_scene->width-256;pce_metrics.camera_x=camera;
    pce_campaign.boss_kind=kind;
    boss_time=0;boss_x=camera+240;boss_y=80;
    if(kind==3){pce_campaign.boss_hp=boss_max=30;boss_phase=3;}
    else overlay_call(0x7b,boss_start);   /* the flying bosses: boss_pce.c */
    audio_music(8);
}
PCE_COMBAT static void power_strike(void) {
    if(hero<2) {
        for(uint8_t k=0;k<8;++k)if(actors[k].active&&!(actors[k].type>=12&&actors[k].type<=28)){actors[k].active=0;++pce_campaign.score;}
        if(pce_campaign.boss_hp) {
            uint16_t maximum=boss_max;
            uint16_t damage=(maximum*(hero?25:18)+99)/100;
            pce_campaign.boss_hp=pce_campaign.boss_hp>damage?pce_campaign.boss_hp-damage:0;
        }
        if(!hero){for(uint8_t k=0;k<NSHOTS;++k)if(shots[k].enemy)shots[k].active=0;safe_timer=150;}
    } else pce_campaign.boost=hero==2?480:600;
    pce_campaign.power_cd=1200;audio_effect(4);
}
/* Moves the camera 4 px towards a target; true on arrival. */
PCE_COMBAT static bool cut_pan(uint16_t target) {
    if(camera+4<=target)camera+=4;else if(camera>=target+4)camera-=4;else {camera=target;return true;}
    return false;
}
PCE_COMBAT static void cut_step(void) {
    uint16_t limit=play_scene->width-256;
    switch(cut_phase) {
    case 1:if((player.coll&4)&&!slide_time){
            int16_t t=dialog_zones[cut_k].focus-128;
            cut_target=t<0?0:t>(int16_t)limit?limit:t;cut_phase=2;
        }break;
    case 2:if(cut_pan(cut_target)){cut_phase=3;cut_wait=dialog_zones[cut_k].before;}break;
    case 3:if(cut_wait)--cut_wait;else {cut_phase=4;pce_campaign.event=1;if(dialog_zones[cut_k].voice)audio_effect(dialog_zones[cut_k].voice);}break;
    case 4:cut_phase=5;cut_wait=dialog_zones[cut_k].after;break;   /* the text has closed */
    case 5:if(cut_wait)--cut_wait;else cut_phase=6;break;
    default:{
            uint16_t t=player.x>120?player.x-120:0;
            if(cut_pan(t>limit?limit:t))cut_phase=0;
        }
    }
}
PCE_COMBAT void combat_tick(void) {
    if(pce_campaign.boost)--pce_campaign.boost;
    if(pce_campaign.power_cd)--pce_campaign.power_cd;
    if(cut_phase){cut_step();return;}
    if((pce_control.pressed&KEY_SELECT)&&(pce_control.keys&KEY_1)&&
       pce_campaign.powers&&!pce_campaign.power_cd&&!pce_campaign.boost) {
        --pce_campaign.powers;pce_campaign.state=CAM_POWER;pce_campaign.timer=0;return;
    }
    for(uint8_t k=0;k<ndialog;++k)if(!(dialogs_done&(1<<k))&&zone(dialog_zones[k].zone)) {
        dialogs_done|=1<<k;pce_campaign.story=k;
        if(dialog_zones[k].focus){cut_k=k;cut_phase=1;overlay_call(0x6f,encounters);}   /* the scene's own actors exist from the start (idle while the camera pans) */
        else {pce_campaign.event=1;if(dialog_zones[k].voice)audio_effect(dialog_zones[k].voice);}
        return;
    }
    for(uint8_t k=0;k<ndeath;++k)if(zone(death_zones[k])&&!safe_timer) {
        campaign_hurt();safe_timer=120;
        if(pce_death){safe_x=death_zones[k][4];safe_y=death_zones[k][5];}
        else{player.x=death_zones[k][4];player.y=death_zones[k][5];}
    }
    if(pce_metrics.stage==4&&pce_campaign.boss_round==2) {
        for(uint8_t k=0;k<8;++k)if(actors[k].active&&!(actors[k].type>=12&&actors[k].type<=28))return;
        pce_campaign.result=1;pce_campaign.story=2;pce_campaign.event=1;return;
    }
    if(!pce_campaign.boss_kind) {
        uint8_t stage=pce_metrics.stage;
        uint16_t start=stage==1?9791:stage==3?play_scene->width-136:stage==5?6558:play_scene->width-140;
        if(stage==3&&player.x>=6480&&!(dialogs_done&16)){dialogs_done|=16;pce_campaign.story=1;pce_campaign.event=1;return;}
        if(player.x>=(int16_t)start) {
            if(stage==4&&++arena_time<1440) {
                if(arena_time==1){pce_campaign.story=1;pce_campaign.event=1;}
                if(!(arena_time%120))for(uint8_t k=0;k<8;++k)if(!actors[k].active) {
                    actors[k]=(Actor){.b={.x=camera+240,.y=170},.active=1,.type=2,.hp=1,.flip=1,.aim=4,.mode=1};break;
                }
            } else {
                boss_begin(stage==1||stage==5?1:2);
                
            }
        }
    } else {
        uint8_t kind=pce_campaign.boss_kind;
        if(kind==3) {
            /* Dark April: runs at the hero's heels and fires level */
            ++boss_time;if(boss_flash)--boss_flash;
            boss_x+=player.x<boss_x?-1:1;
            if(boss_x<(int16_t)camera+32)boss_x=camera+32;
            if(boss_x>(int16_t)camera+224)boss_x=camera+224;
            boss_y=player.y;
            if(!(boss_time%45))bullet(boss_x,boss_y-8,(player.x<boss_x?-4:4)<<8,0);
            if(!safe_timer&&distance(player.x,boss_x)<14&&distance(player.y,boss_y)<28){safe_timer=120;campaign_hurt();}
            for(uint8_t k=0;k<NSHOTS;++k) {
                Shot *s=&shots[k];if(!s->active||s->enemy)continue;
                if(distance(s->x,boss_x)<12&&distance(s->y,boss_y)<24) {
                    s->active=0;if(pce_campaign.boss_hp)--pce_campaign.boss_hp;boss_flash=6;audio_effect(4);
                }
            }
        } else overlay_call(0x7b,boss_tick);
        if(!pce_campaign.boss_hp) {
            if(pce_metrics.stage==4) {
                ++pce_campaign.score;pce_campaign.boss_round=2;
            } else if(pce_metrics.stage==5&&kind==1) {
                pce_campaign.boss_round=1;boss_begin(3);
                pce_campaign.story=hero==2?5:2;pce_campaign.event=1;
            } else {
                ++pce_campaign.score;pce_campaign.result=1;
                if(pce_metrics.stage!=1){pce_campaign.story=pce_metrics.stage==5?(hero==2?6:3):2;pce_campaign.event=1;}
            }
        }
    }
    /* The robot-horse herd tramples every humanoid in its way, as in the main game. */
    uint8_t humanoids=0,horses=0;
    for(uint8_t k=0;k<8;++k)if(actors[k].active) {
        uint8_t t=actors[k].type;
        if(t==11)++horses;else if(!actors[k].dead&&!(t>=12&&t<=28))++humanoids;
    }
    if(horses&&humanoids)for(uint8_t k=0;k<8;++k)if(actors[k].active&&actors[k].type==11)
        for(uint8_t j=0;j<8;++j)if(actors[j].active&&!actors[j].dead&&actors[j].type!=11&&!(actors[j].type>=12&&actors[j].type<=28)
            &&distance(actors[j].b.x,actors[k].b.x)<48&&distance(actors[j].b.y,actors[k].b.y)<40)actor_kill(&actors[j]);
    if(horses||humanoids)for(uint8_t k=0;k<8;++k)if(actors[k].active&&!actors[k].dead&&!(actors[k].type>=12&&actors[k].type<=28)&&distance(player.x,actors[k].b.x)<(actors[k].type==11?44:14)&&distance(player.y,actors[k].b.y)<(actors[k].type==11?40:24)) {
        if(slide_time)actor_kill(&actors[k]);      /* a slide knocks enemies down */
        else if(!safe_timer){safe_timer=120;campaign_hurt();}
    }
}
PCE_COMBAT void combat_draw(void) {
    if(pce_campaign.state==CAM_POWER) {
        const char *name=hero==0?"SABER SLASH":hero==1?"FIREBALL BLAST":hero==2?"APRIL OVERDRIVE":"COLT RAPID FIRE";
        video_text(6,12,name);
        pce_campaign.timer+=pce_control.elapsed;
        if(pce_campaign.timer>=114){power_strike();pce_campaign.state=CAM_PLAY;video_restore();}
    }
    if(pce_campaign.boss_kind==3&&pce_campaign.boss_hp&&(!boss_flash||(frame&2)))video_sprite(43,boss_x-camera,boss_y-16,player.x<boss_x,16);
    else if(pce_campaign.boss_kind&&pce_campaign.boss_kind!=3&&pce_campaign.boss_hp)overlay_call(0x7b,boss_draw);
    if(pce_campaign.boss_kind){video_text(1,1,"BOSS");video_number(6,1,pce_campaign.boss_hp);}

}
