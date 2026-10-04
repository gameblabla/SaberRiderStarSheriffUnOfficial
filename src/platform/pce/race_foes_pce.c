#include "campaign_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
#define RACE_SIN_SECTION ".ram_bank122.rodata"
#include "race_math.h"
#include "race_pce.h"
#include <string.h>
/* Shots, mines, the Hornet leader and his escort (mode7.c update_ents), called from the race's step. */
#define FOES_CODE PCE_RACE2
static uint8_t rnd(void) {
    uint8_t carry=race_rng&1;race_rng>>=1;if(carry)race_rng^=0xB400;
    return (uint8_t)(race_rng^(race_rng>>8));
}
FOES_CODE static void hurt_car(uint8_t damage) {arg_damage=damage;overlay_call(0x79,hurt_call);}
FOES_CODE static void bump(int16_t x,int16_t y,int16_t dist,int16_t r) {arg_x=x;arg_y=y;arg_dist=dist;arg_radius=r;overlay_call(0x6d,bump_call);}
FOES_CODE static void wreck(void) {audio_effect(4);}
FOES_CODE static void set_pos(Leader *e) {e->x=(e->xq>>8)&8191;e->y=(e->yq>>8)&8191;}
FOES_CODE void foes_drop_mine(void) {
    for(uint8_t m=0;m<6;++m)if(!mines[m].t){mines[m]=(Mine){arg_x,arg_y,arg_life};return;}
}
/* A shot from (arg_x, arg_y) at the car: speed in Q8 units a step, a random spread of about +-0.2 radians. */
FOES_CODE void foes_aimed_bolt(void) {
    for(uint8_t b=0;b<6;++b)if(!race_bolts[b].t) {
        int16_t dx=wrapdiff(px,arg_x),dy=wrapdiff(py,arg_y);
        int16_t dist=hypot16(dx,dy);if(dist<1)dist=1;
        int16_t jit=(int16_t)(rnd()%102)-51;
        int16_t jx=dx-(dy>>3)*jit/32,jy=dy+(dx>>3)*jit/32;
        /* the direction as dx / dist in Q5, times the speed: (|jx| << 5) / dist * (speed / 8) / 4 */
        uint16_t ux=(uint16_t)absolute(jx)<<5,uy=(uint16_t)absolute(jy)<<5;
        int16_t vx=(int16_t)((ux/dist)*(uint16_t)(arg_speed>>2)>>3),vy=(int16_t)((uy/dist)*(uint16_t)(arg_speed>>2)>>3);
        race_bolts[b]=(Bolt){arg_x,arg_y,jx<0?-vx:vx,jy<0?-vy:vy,arg_life,0};
        audio_effect(1);
        return;
    }
}
FOES_CODE void foes_leader_start(void) {
    memset(escort,0,sizeof escort);memset(mines,0,sizeof mines);memset(race_bolts,0,sizeof race_bolts);
    memset(&boss,0,sizeof boss);
    boss.hp=boss.hp_max=pce_options.difficulty==0?60:pce_options.difficulty==1?80:100;
    boss.xq=(int32_t)4096<<8;boss.yq=((int32_t)py-1500)<<8;boss.speed=300;boss.since=600;boss.t2=180;
    set_pos(&boss);
}
FOES_CODE void foes_spawn_escort(void) {
    for(uint8_t k=0;k<2;++k)if(!escort[k].hp) {
        Leader *e=&escort[k];
        memset(e,0,sizeof *e);
        e->xq=(int32_t)(4096+(int16_t)(rnd()%160)-80)<<8;e->yq=((int32_t)py-900-(int16_t)(rnd()%300))<<8;
        e->speed=300;e->hp=e->hp_max=4+pce_options.difficulty;e->t=(uint8_t)(60+rnd()%120);
        set_pos(e);return;
    }
}
FOES_CODE void foes_shots(void) {
    for(uint8_t k=0;k<6;++k) {
        Bolt *b=&race_bolts[k];if(!b->t)continue;
        b->x=(b->x+(b->vx>>8))&8191;b->y=(b->y+(b->vy>>8))&8191;
        --b->t;
        if(b->own) {
            for(uint8_t i=0;i<7&&b->t;++i) {
                Rival *r=&rv[i];
                if(r->hp&&absolute(wrapdiff(b->x,r->x))<34&&absolute(wrapdiff(b->y,r->y))<34) {
                    b->t=0;r->knock=14;audio_effect(7);
                    if(!--r->hp)wreck();
                }
            }
            if(rphase>=P_PURSUIT) {
                for(uint8_t i=0;i<2&&b->t;++i)if(escort[i].hp&&absolute(wrapdiff(b->x,escort[i].x))<34&&absolute(wrapdiff(b->y,escort[i].y))<34) {
                    b->t=0;audio_effect(7);if(!--escort[i].hp)wreck();
                }
                if(b->t&&boss.state<2&&absolute(wrapdiff(b->x,boss.x))<42&&absolute(wrapdiff(b->y,boss.y))<42) {
                    b->t=0;boss.knock=18;audio_effect(7);
                    if(boss.state==1&&!--boss.hp){boss.state=2;boss.t=144;boss.t2=0;wreck();}   /* a hit only slows him while he flees */
                }
            }
            for(uint8_t i=0;i<6&&b->t;++i)
                if(mines[i].t&&absolute(wrapdiff(b->x,mines[i].x))<18&&absolute(wrapdiff(b->y,mines[i].y))<18){b->t=0;mines[i].t=0;wreck();}
        } else if(absolute(wrapdiff(b->x,px))<22&&absolute(wrapdiff(b->y,py))<22){b->t=0;hurt_car(1);}
    }
    for(uint8_t k=0;k<6;++k) {
        Mine *m=&mines[k];if(!m->t)continue;
        --m->t;
        if(absolute(wrapdiff(m->x,px))<26&&absolute(wrapdiff(m->y,py))<26) {
            m->t=0;wreck();hurt_car(2);
            if(hurt){speed=speed*2/5;spin=36;}
        }
    }
}
FOES_CODE void foes_escorts(void) {
    for(uint8_t k=0;k<2;++k) {
        Leader *e=&escort[k];if(!e->hp)continue;
        int16_t dx=wrapdiff(px,e->x),dy=wrapdiff(py,e->y);
        int16_t dist=hypot16(dx,dy);
        int16_t step=e->speed*4+(e->speed>>2);   /* Q8 units a step */
        if(e->state==0) {   /* running, weaving across the road */
            e->anim+=226;
            e->speed+=(300-e->speed)>>5;
            e->xq+=muls(step,sine(e->anim))*35/100;
            if(e->t)--e->t;else{e->state=1;e->t=(uint8_t)(150+rnd()%90);}
        } else {            /* slowed right down, guns on the car */
            e->anim+=680;
            e->speed+=(90-e->speed)>>4;
            if(e->t2)--e->t2;
            else if(dist<1400){e->t2=66;arg_x=e->x;arg_y=e->y;arg_speed=2560;arg_life=140;foes_aimed_bolt();}
            if(e->t)--e->t;else{e->state=0;e->t=(uint8_t)(180+rnd()%180);}
        }
        if(e->knock){--e->knock;e->speed=e->speed*97/100;}
        e->yq-=step;set_pos(e);
        if(dist<44){bump(e->x,e->y,dist,44);hurt_car(1);speed=speed*3/5;}
        if(dy<-1800||dy>5000)e->hp=0;
    }
}
FOES_CODE void foes_leader(void) {
    Leader *e=&boss;
    if(e->state==3)return;
    if(e->knock)--e->knock;
    int16_t dx=wrapdiff(px,e->x),gap=wrapdiff(py,e->y);
    int16_t dist=hypot16(dx,gap),target=0;
    e->anim+=e->state==1?243:156;
    if(e->state==2) {   /* burning out: rolls to a stop, sparks, then the big one */
        e->speed-=e->speed>>4;
        if(!(e->t&7))wreck();
        if(!--e->t){wreck();e->state=3;rphase=P_VICTORY;phase_t=0;pce_campaign.boss_hp=0;return;}
    } else if(e->state==0) {   /* the pursuit: up the road, pace rubber-banded to the gap so he stays in reach but never free */
        target=gap>3400?250:gap>2200?380:(gap<600?540:470);
        if(e->knock&&!e->boost)target=target*3/4;
        if(!e->boost&&e->since>180&&gap<560&&gap>0){e->boost=210;e->speed=700;wreck();}   /* early in the chase he always has one more booster */
        if(e->boost){--e->boost;target=700;e->since=0;}else if(e->since<1000)++e->since;
        e->speed+=(target-e->speed)>>(e->boost?4:6);
        if(e->t2)--e->t2;
        else if(gap<900&&gap>0){e->t2=(uint8_t)(84+rnd()%60);arg_x=e->x;arg_y=e->y;arg_life=200;foes_drop_mine();}
    } else {   /* the fight: he keeps racing just ahead of the car, weaving to block, mines out the back, a rear gunner, a booster now and then */
        target=gap<0?560:gap<140?600:gap<520?445:gap<1200?360:280;
        if(e->knock)target=target*4/5;
        if(e->t3)--e->t3;
        else if(gap>0&&gap<450&&!e->boost){e->boost=138;e->t3=(uint8_t)(120+rnd()%120);e->speed=700;wreck();}
        if(e->boost){--e->boost;target=700;}
        e->speed+=(target-e->speed)>>(e->boost?4:6);
        if(e->t2)--e->t2;
        else if(gap>60&&gap<700){e->t2=(uint8_t)(60+rnd()%48);arg_x=e->x+(int16_t)(rnd()%40)-20;arg_y=e->y;arg_life=220;foes_drop_mine();}
        if(e->since)--e->since;
        else if(gap>90&&gap<1500){e->since=(uint8_t)(72+rnd()%36);arg_x=e->x;arg_y=e->y;arg_speed=2560;arg_life=140;foes_aimed_bolt();}
    }
    /* weaving about the road (jinking out of the car's line when it closes in); the heading follows it */
    int16_t road_x=4096+muls(e->state==1?85:60,sine(e->anim));
    if(e->state==1&&gap>0&&gap<260){int16_t j=dx;if(j>60)j=60;if(j<-60)j=-60;road_x-=j;}
    int16_t angle=wrapdiff(road_x,e->x)*(e->state==1?3:2)>>1;   /* radians in Q8: 0.006 / 0.004 a unit */
    int16_t limit=e->state==1?128:102;
    if(angle>limit)angle=limit;if(angle<-limit)angle=-limit;
    int16_t step=e->speed*4+(e->speed>>2);
    e->yq-=step;e->xq+=muls(step,angle);set_pos(e);
    if(e->state<2&&dist<70) {   /* contact: a fast ram from behind dents him, a side-swipe just costs speed */
        bump(e->x,e->y,dist,70);
        if(!ram_cd) {
            ram_cd=30;
            if(speed>330&&gap>0&&e->state==1) {
                speed=speed*55/100;shake=21;wreck();
                if(e->hp>4)e->hp-=4;else{e->hp=0;e->state=2;e->t=144;e->t2=0;}
            } else speed=speed*3/5;
        }
    }
}
