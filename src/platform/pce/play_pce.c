#include "play_internal.h"
#include "presentation_pce.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include <string.h>

/* Positions retain whole-world range. Fractions and velocities are separate
 * Q8 values; a 16-bit fixed-point world coordinate would overflow after 127px. */
typedef struct __attribute__((packed)) {
    int16_t cx,cy,hx,hy; uint16_t interval,delay,type;
    uint8_t layer; int8_t remaining; uint8_t nwp; int16_t wp[8][2];
} Trigger;
_Static_assert(sizeof(Trigger)==49,"Trigger format changed");
Body player;
Actor actors[8] PCE_WORK;
Shot shots[24] PCE_WORK;
static uint8_t collision[32][32] PCE_WORK;
static uint16_t column_tags[32];
static uint16_t trigger_timers[100];
static int8_t trigger_remaining[100];
static uint8_t trigger_spawned[100];
static Trigger trigger;
static Trigger trigger_cache[60] PCE_STAGE;
const PceScene *play_scene;
#define scene play_scene
uint16_t camera,frame;
uint8_t hero,facing,safe_timer;
static uint8_t fire_timer,crouch,slide_time;
static int16_t safe_x,safe_y,drop_y;

PCE_CODE static uint8_t cell(int16_t x,int16_t y) {
    if(x<0||(uint16_t)x>=scene->ccols||y<0||(uint16_t)y>=scene->crows) return 0;
    uint8_t slot=x&31;
    if(column_tags[slot]!=(uint16_t)x) {
        arcade_read(1,scene->collision+(uint32_t)(uint16_t)x*scene->crows,collision[slot],scene->crows);
        column_tags[slot]=x;
    }
    return collision[slot][y];
}
PCE_CODE static void advance(int16_t *p,uint8_t *fraction,int16_t velocity) {
    int16_t sum=(int16_t)*fraction+velocity;
    *p+=sum>>8;*fraction=sum;
}
PCE_CODE static void physics(Body *b) {
    uint8_t prior=b->coll,ground=b->ground;
    b->vy+=34;if(b->vy>2560) b->vy=2560;
    advance(&b->x,&b->fx,b->vx);advance(&b->y,&b->fy,b->vy);
    int16_t x=b->x+4,y=b->y+9;
    uint8_t hit=0,tile=0;
    int16_t r0=(y-23)>>3,r1=(y+23)>>3;
    if(b->vx>0) {
        int16_t col=(x+8)>>3;
        if(x+8>scene->width) { x=scene->width-8;hit|=1;b->vx=0; }
        else for(int16_t r=r0;r<r1;++r) if(cell(col,r)&1) {x=col*8-8;b->vx=0;hit|=1;break;}
    } else if(b->vx<0) {
        int16_t col=(x-8)>>3;
        if(x-8<0) {x=8;b->vx=0;hit|=2;}
        else for(int16_t r=r0;r<r1;++r) if(cell(col,r)&2) {x=(col+1)*8+8;b->vx=0;hit|=2;break;}
    }
    int16_t c0=(x-7)>>3,c1=(x+7)>>3,last=c1-(c0<c1),row;
    if(b->vy<0) {
        row=(y-23)>>3;
        for(int16_t c=c0;c<=last;++c) if(cell(c,row)&8) {y=(row+1)*8+23;b->vy=0;hit|=8;break;}
    } else if(b->vy>0&&!(b==&player&&b->y<drop_y)) {
        row=(y+23)>>3;
        for(int16_t c=c0;c<=last;++c) {
            uint8_t v=cell(c,row);
            if(v&4) {
                if(v&16) for(uint8_t k=0;k<2;++k) if((cell(c,row-1)&20)==20) {--row;v=cell(c,row);}
                y=row*8+1-23;b->vy=0;hit|=4;tile=v;break;
            }
        }
        if(!(hit&4)&&(prior&4)&&(ground&16)) for(int16_t c=c0;c<=last;++c)
            if((cell(c,row+1)&20)==20) {y=(row+1)*8+1-23;b->vy=0;hit|=4;tile=cell(c,row+1);break;}
    }
    b->x=x-4;b->y=y-9;b->coll=hit;b->ground=tile;
}
PCE_CODE static void shoot(int16_t x,int16_t y,int16_t vx,int16_t vy,bool enemy) {
    uint8_t active=0;
    for(uint8_t k=0;k<24;++k)if(shots[k].active&&shots[k].enemy==enemy)++active;
    if(active>=(enemy?(pce_campaign.boss_kind?2:6):4))return;
    for(uint8_t k=0;k<24;++k) if(!shots[k].active) {
        shots[k]=(Shot){x,y,vx,vy,1,enemy};return;
    }
}
PCE_CODE void play_init(uint8_t stage,uint8_t selected) {
    scene=&pce_scenes[stage-1];hero=selected;camera=frame=0;
    facing=fire_timer=safe_timer=crouch=slide_time=0;drop_y=-32767;
    player=(Body){.x=scene->sx,.y=scene->sy};safe_x=player.x;safe_y=player.y;
    memset(actors,0,sizeof actors);memset(shots,0,sizeof shots);
    memset(column_tags,0xff,sizeof column_tags);
    for(uint8_t k=0;k<scene->ntr;++k) {
        arcade_read(2,scene->triggers+(uint32_t)k*sizeof trigger,&trigger,sizeof trigger);
        if(k<60) trigger_cache[k]=trigger;
        trigger_timers[k]=trigger.delay;trigger_remaining[k]=trigger.remaining;trigger_spawned[k]=0;
    }
    pce_metrics.hp=3;
    overlay_call(0x70,combat_start);
}
PCE_CODE static void encounters(void) {
    if(pce_metrics.stage==4&&pce_campaign.boss_round==2)return;
    uint8_t k=0;
    for(const Trigger *t=trigger_cache;k<scene->ntr;++k,++t) {
        if(!trigger_remaining[k]) continue;
        if(t->type==10)continue;
        if(player.x+4<t->cx-t->hx-8||player.x+4>t->cx+t->hx+8||
           player.y+9<t->cy-t->hy-23||player.y+9>t->cy+t->hy+23) continue;
        if(trigger_timers[k]) {--trigger_timers[k];continue;}
        /* Humanoid core first; the inventory retains other encounter recipes
         * for their stage-specific handlers rather than replacing their art. */
        for(uint8_t i=0;i<8;++i) if(!actors[i].active) {
            uint8_t wp=trigger_spawned[k]%t->nwp;
            int16_t x=t->wp[wp][0],y=t->wp[wp][1];
            if(x>30000)x=camera+288;else if(x< -30000)x=camera-32;
            if(y>30000)y=256;else if(y< -30000)y=-32;
            if(y< -999)y=-1000-y;
            actors[i]=(Actor){.b={.x=t->type>=11&&t->type<=27?x:x+8,.y=t->type>=11&&t->type<=27?y:y+19},.active=1,.type=t->type,.hp=t->type>=30?6:2,.timer=60,.flip=player.x<x};
            if(trigger_remaining[k]>0)--trigger_remaining[k];
            ++trigger_spawned[k];trigger_timers[k]=t->interval;break;
        }
    }
}
PCE_CODE void play_tick(uint8_t keys,uint8_t pressed) {
    ++frame;
    if(!pce_campaign.diagnostic){overlay_call(0x70,combat_tick);
        if(pce_campaign.state!=CAM_PLAY||pce_campaign.event)return;}
    if(safe_timer)--safe_timer;if(fire_timer)--fire_timer;
    player.vx=0;crouch=(keys&KEY_DOWN)&&(player.coll&4);
    if(!(keys&KEY_SELECT)&&!crouch) {
        if(keys&KEY_LEFT){player.vx=pce_campaign.boost&&hero==2?-726:-427;facing=1;}
        else if(keys&KEY_RIGHT){player.vx=pce_campaign.boost&&hero==2?726:427;facing=0;}
    }
    if((pressed&KEY_2)&&(player.coll&4)) {
        if(crouch&&player.ground==4)drop_y=player.y+20;
        else if(crouch)slide_time=24;
        else {player.vy=-1237;audio_effect(2);}
    }
    if(keys&KEY_SELECT) {
        if(keys&KEY_LEFT)facing=1;
        if(keys&KEY_RIGHT)facing=0;
    }
    if(slide_time){crouch=1;player.vx=(int16_t)slide_time*43;if(facing)player.vx=-player.vx;--slide_time;}
    physics(&player);
    if(pce_campaign.boss_kind){if(player.x<(int16_t)camera+8)player.x=camera+8;if(player.x>(int16_t)camera+248)player.x=camera+248;}
    if(player.coll&4){safe_x=player.x;safe_y=player.y;}
    if(player.y>272){campaign_hurt();player=(Body){.x=safe_x,.y=safe_y};safe_timer=120;}
    if((keys&KEY_1)&&!fire_timer) {
        int16_t vx=facing?-8:8,vy=0;
        if(keys&KEY_UP){vy=-8;if(!(keys&(KEY_RIGHT|KEY_LEFT)))vx=0;}
        if((keys&KEY_DOWN)&&(!crouch||(keys&(KEY_SELECT|KEY_LEFT|KEY_RIGHT)))){vy=8;if(!(keys&(KEY_LEFT|KEY_RIGHT)))vx=0;}
        shoot(player.x+(facing?-18:18),player.y+(crouch?4:-8),vx,vy,false);fire_timer=pce_campaign.boost&&hero==3?4:12;audio_effect(1);
    }
    encounters();
    for(uint8_t k=0;k<8;++k) {
        Actor *a=&actors[k];if(!a->active)continue;
        if(a->b.x<(int16_t)camera-80||a->b.x>(int16_t)camera+384||a->b.y>272){a->active=0;continue;}
        if(a->type>=12&&a->type<=23)continue;
        if(a->type>=24&&a->type<=27) {
            const int16_t speeds[4]={2016,1142,2352,1344};
            advance(&a->b.x,&a->b.fx,a->flip?-speeds[a->type-24]:speeds[a->type-24]);continue;
        }
        if(a->type==11){advance(&a->b.x,&a->b.fx,a->flip?-640:640);continue;}
        if(a->type==28){if(a->timer)--a->timer;else a->b.x-=2;continue;}
        a->flip=player.x<a->b.x;
        a->b.vx=a->type<6?(a->flip?-170:170):0;
        if(a->timer>45)a->b.vx=0;
        physics(&a->b);
        if(a->timer)--a->timer;else if(a->type>=2&&(a->type!=31||pce_campaign.boss_kind)) {
            shoot(a->b.x+(a->flip?-16:16),a->b.y-8,a->flip?-3:3,0,true);a->timer=90;
        }
    }
    for(uint8_t k=0;k<24;++k) {
        Shot *s=&shots[k];if(!s->active)continue;
        s->x+=s->vx;s->y+=s->vy;
        if(s->x<(int16_t)camera-16||s->x>(int16_t)camera+272||s->y<0||s->y>240){s->active=0;continue;}
        if(s->enemy) {
            if(!safe_timer&&s->x>player.x-8&&s->x<player.x+8&&s->y>player.y-14&&s->y<player.y+25) {
                s->active=0;safe_timer=120;audio_effect(5);
                campaign_hurt();
                if(pce_campaign.diagnostic&&!pce_metrics.hp)pce_metrics.hp=3;
            }
        } else for(uint8_t j=0;j<8;++j) {
            Actor *a=&actors[j];
            if(a->active&&!(a->type>=12&&a->type<=28)&&s->x>a->b.x-10&&s->x<a->b.x+10&&s->y>a->b.y-18&&s->y<a->b.y+24) {
                s->active=0;if(a->type>=30&&facing==a->flip)a->hp=1;
                if(!--a->hp){a->active=0;++pce_campaign.score;}audio_effect(4);break;
            }
        }
    }
    camera=pce_campaign.boss_kind?scene->width-256:player.x>120?player.x-120:0;
    if(camera>(uint16_t)(scene->width-256))camera=scene->width-256;
    pce_metrics.player_x=player.x;pce_metrics.player_y=player.y;pce_metrics.camera_x=camera;pce_metrics.hero=hero;
}
__attribute__((noinline)) void play_draw(void) {
    video_background(camera);video_sat_begin();presentation_draw();
    uint8_t pose=crouch?8:!(player.coll&4)?7:player.vx?1+(frame/6)%6:0;
    uint16_t id=hero*9+pose;
    if((pce_control.keys&KEY_UP)||((pce_control.keys&KEY_DOWN)&&(!crouch||(pce_control.keys&(KEY_SELECT|KEY_LEFT|KEY_RIGHT))))) {
        uint8_t direction=pce_control.keys&KEY_UP?0:1;
        id=pce_present_base[pce_metrics.stage-1][2]+hero*14+direction;
        if(player.vx)id+=2+direction*5+(frame/6)%6;
        if(pce_control.keys&KEY_LEFT)facing=1;
        if(pce_control.keys&KEY_RIGHT)facing=0;
    }
    if(!safe_timer||(frame&4))video_sprite(id,player.x-camera,player.y-16,facing,16);
    /* Essential projectiles precede optional distant enemies. */
    for(uint8_t k=0;k<24;++k) if(shots[k].active)
        video_sprite(shots[k].enemy?37:36,shots[k].x-camera,shots[k].y-16,false,16);
    if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
    for(uint8_t k=0;k<8;++k) if(actors[k].active) {
        Actor *a=&actors[k];uint16_t id=pce_actor_ids[a->type];
        if(id==255)continue;
        if(!video_sprite_optional(id,a->b.x-camera,a->b.y-16,a->flip,16))a->active=0;
    }
    foreground_draw();video_sat_end();
}
