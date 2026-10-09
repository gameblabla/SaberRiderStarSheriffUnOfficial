#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
#include "sgx_pce.h"
/* The world's moving parts: enemies, one step at a time (shots_pce.c has the shots). The behaviours are the source
 * game's (enemies.c) in whole steps of 1/60 s:
 *   walker  walks the way it spawned, turns at walls and cars;
 *   grunt   walks; with the hero ahead of it (over 32 px away, on screen) 4% of the steps it stops, fires ONE shot on the
 *           16th step of a 24-step stand and walks on; it never fires again;
 *   sniper  stands and aims at the hero in eight directions; once the aim has settled it fires with 1% a step;
 *   kneeler crouches facing the hero and lobs a grenade (7% a step once ready, 24 steps of wind-up, ~28 of recovery);
 *   shield  stands, takes the hero's fire on its shield (hp hits), shoots level at the pace of the source's
 *           stage-4 sniper.
 * Distances are kept in bytes where they can be (saturated): the 8-bit forms are what keeps this inside its bank. */
#define ENEMY_CODE __attribute__((noinline,section(".ram_bank115.text")))
extern Body *phys_body;
void shots_step(void);
void phys_call(void);

extern uint16_t rng;
ENEMY_CODE static uint8_t rnd(void) {
    uint8_t carry=rng&1;rng>>=1;if(carry)rng^=0xB400;
    return (uint8_t)(rng^(rng>>8));
}
ENEMY_CODE static void phys(Body *b) {phys_body=b;overlay_call(0x81,phys_call);}
ENEMY_CODE static void advance(int16_t *p,uint8_t *f,int16_t v) {
    int16_t sum=(int16_t)*f+v;
    *p+=sum>>8;*f=sum;
}
ENEMY_CODE static uint8_t distance8(int16_t a,int16_t b) {
    int16_t n=a-b;if(n<0)n=-n;
    return n>255?255:n;
}

/* The barrel of each aim (body-relative, as the source's standing aim animations): L UL U UR R DR D DL. */
static const int8_t muzzle[8][2] __attribute__((section(".ram_bank115.rodata")))={
    {-25,-1},{-17,-20},{-1,-31},{18,-20},{25,-1},{13,15},{2,31},{-14,15}};
/* Spawning an enemy shot or grenade runs in the platform bank (play_pce.c), beside the hero's own shoot(). */
extern const Actor *fire_actor;
extern uint8_t fire_aim;
extern int8_t fire_mx,fire_my;
extern bool fire_fast;
void fire_call(void);
void grenade_call(void);
ENEMY_CODE static void fire(const Actor *a,uint8_t aim,int8_t mx,int8_t my,bool fast) {
    fire_actor=a;fire_aim=aim;fire_mx=mx;fire_my=my;fire_fast=fast;overlay_call(0x69,fire_call);
}
ENEMY_CODE static void grenade(const Actor *a) {fire_actor=a;overlay_call(0x69,grenade_call);}
ENEMY_CODE static void humanoid(Actor *a) {
    uint8_t type=a->type;
    uint8_t kind=type==2||type==5?1:type==8||type==9?3:type==30||type==31?4:type>=6?2:0;
    int16_t px=player.x,ax=a->b.x;
    bool onscreen=ax>(int16_t)camera&&ax<(int16_t)camera+256;
    /* An enemy dropped in from its spawn point (character.c CF_SPAWN_FALL): it was thrown up at 167 px/s and does nothing until it lands */
    if(a->mode&8){a->b.vx=0;phys(&a->b);if(a->b.coll&4)a->mode&=~8;return;}
    if(kind<2) {
        if(a->timer) {
            a->b.vx=0;phys(&a->b);
            ++a->timer;
            if(a->timer==16&&(a->mode&1))fire(a,a->flip?0:4,a->flip?(type==5?-29:-26):(type==5?29:26),-1,false);
            else if(a->timer>=24){a->timer=0;a->mode&=~1;}
            return;
        }
        a->b.vx=a->flip?-512:512;phys(&a->b);
        if(a->b.coll&3){a->flip^=1;a->mode&=~1;}
        if(a->b.vx)++a->anim;
        if(kind&&(a->b.coll&4)&&(a->mode&1)&&rnd()>=246&&
           (a->flip?px+32<ax&&ax<(int16_t)camera+224:px-32>ax&&ax>(int16_t)camera+32))a->timer=1;
        return;
    }
    a->b.vx=0;phys(&a->b);
    bool grounded=a->b.coll&4;
    if(kind==2) {
        if(!grounded)return;
        uint8_t t=a->timer;
        if(t<=12)++t;
        else {
            int16_t ay=a->b.y,py=player.y+8,lift=0;
            /* a hero in mid-jump is led a little: the source adds 20 px while he is rising slowly or just topping */
            if(!(player.coll&4)&&player.vy>-640&&player.vy<298)lift=20;
            uint8_t dx=distance8(px,ax),dyq=distance8(py+lift,ay)&0xf0;
            bool below=ay<py,right=ax<=px;
            uint8_t want,settle=7;
            if(dyq<dx||dx>16) {
                want=(below?24:48)<dyq||(dx&&dyq>>1>dx)?(below?(right?5:7):(right?3:1)):(right?4:0);settle=1;
            } else if(!dyq||(uint16_t)((dx<<3)+(dx<<1))<=dyq)want=below?6:2;
            else want=below?(right?5:7):(right?3:1);
            if(a->aim!=want){a->aim=want;t=settle;}
            else if(t>20) {
                if(onscreen&&rnd()>=253){
                    fire(a,want,muzzle[want][0],muzzle[want][1],false);
                    t=0;a->anim=12;
                }
            } else ++t;
        }
        a->timer=t;
        if(a->anim)--a->anim;
        a->flip=!(a->aim==2||(a->aim>=3&&a->aim<=6));
        return;
    }
    if(kind==3) {
        if(!grounded)return;
        a->aim=ax<=px?4:0;a->flip=!a->aim;
        if(!a->timer) {if(onscreen&&rnd()>=238){a->timer=1;a->mode|=2;}}
        else {
            ++a->timer;
            if((a->mode&2)&&a->timer==25){grenade(a);a->mode&=~2;}
            else if(a->timer>=52)a->timer=0;
        }
        return;
    }
    /* shield: faces the hero, rifle level, long pauses */
    a->flip=px<ax;
    if(type==31&&!pce_campaign.boss_kind)return;
    if(a->anim)--a->anim;
    bool burning=(a->mode&4)&&a->aim;   /* the shield is burning away: no shots meanwhile */
    if(burning)--a->aim;
    if(a->timer)--a->timer;
    else if(onscreen&&!burning) {
        fire(a,a->flip?0:4,a->flip?-22:22,-8,true);a->anim=8;
        a->timer=84+rnd()%48-pce_options.difficulty*12;
    }
}
ENEMY_CODE void world_update(void) {
    rng^=frame<<1;if(!rng)rng=1;
    /* Only the walkers and grunts that stream in from the screen edges are dropped when they stray far ahead: the level's
     * placed snipers, kneelers and shields stand where the level puts them (the zone that wakes them is a screen or more behind) */
    for(Actor *a=actors;a<actors+8;++a) {   /* by pointer: indexing a 21-byte record costs a multiplication each time */
        if(!a->active)continue;
        int16_t left=(int16_t)camera-80;
#ifdef PCE_SGX
        /* The added horse starts at actor X - camera - 72 + 112.
         * Keep its last 32px column through X=-127; retire the shared actor
         * only once that entire 128px horse is offscreen (X<=-128). */
        if(a->type==11&&pce_sgx_gameplay())left=(int16_t)camera-167;
#endif
        if(((a->b.x<left||(a->b.x>(int16_t)camera+384&&a->type<6))&&a->type!=28)||a->b.y>272){a->active=0;continue;}
        /* The stampede tramples every humanoid in its way: the placed snipers and the rest are simply gone while it runs, which is
         * also what keeps its scenes inside the sprite budget (they are never drawn, uploaded or fired from). */
        if(herd_on&&(a->type<11||a->type>28)) {
            /* (every stampede: its enemies turn and run off to the left, snipers and kneelers in the run of the brown grunt, which has their body) */
            if(!herd_flee||a->type>10){a->active=0;continue;}
            if(a->dead){if(++a->dead>24)a->active=0;continue;}
            if(a->type>5)a->type=5;
            a->mode&=~8;a->timer=0;a->flip=1;a->b.vx=-768;phys(&a->b);++a->anim;
            if(a->b.coll&3)a->active=0;   /* (a wall in the way: nowhere left to run) */
            continue;
        }
        if(a->type>=12&&a->type<=27)continue;
        if(a->type==11){advance(&a->b.x,&a->b.fx,a->flip?-512:512);continue;}   /* the herd gallops at the source's 120 px/s */
        if(a->type==28) {
            /* The cutscene outrider (hp 2 waiting, 1 alarmed, 0 running): once it is on screen it freezes for a second
             * facing the hero, then turns and runs off to the right. */
            if(a->hp==2){int16_t d=camera+128-a->b.x;if(d<0)d=-d;if(d<128){a->hp=1;a->timer=62;audio_effect(9);}}
            else if(a->hp==1){if(!--a->timer){a->hp=0;a->flip=0;}}
            a->b.vx=a->hp?0:512;phys(&a->b);
            if(!a->hp){++a->anim;if(a->b.x>(int16_t)camera+272)a->active=0;}
            continue;
        }
        if(a->dead){if(++a->dead>24)a->active=0;continue;}
        humanoid(a);
    }
    overlay_call(0x70,shots_step);
}
