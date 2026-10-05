#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
#include "assets.h"
#include <string.h>
/* Stage 5's second boss, Dark April (darkapril.c), in whole 1/60 s steps. She is April's body run through the hero's own physics (phys_call) and
 * moves with "buttons" a small AI holds: she keeps her range (backs off when the hero is close, closes in firing when he is far), holds and aims
 * at his height in between, hops when he hops, and ducks or hops the fire he sends. The scenes are the source's: the gunship's wreck burns out
 * (1.8 s, the music stops), the radio call, she forms out of the motes (1.8 s), the meeting, a beat (0.9 s) in which Ramrod tops the hero up, the
 * fight, her death and dissolve, and the ending scene. Her shots are an enemy's (200 px/s) in the enemies' pool; her art is dark_april in the
 * sprite set (tools/pce/presentation.py). The state lives in the work bank (the console RAM is full) and the code is spread over the banks that
 * have room: the scenes and the body in $71, the AI in $6e, her shots, hits and drawing in $70. */
#define DARK_CODE __attribute__((noinline,minsize,section(".ram_bank113.text")))
#define DARK_AI __attribute__((noinline,minsize,section(".ram_bank110.text")))
#define DARK_HIT __attribute__((noinline,minsize,section(".ram_bank112.text")))
enum { D_WAIT, D_CALL, D_APPEAR, D_MEET, D_READY, D_FIGHT, D_DYING };
typedef struct {Body b;uint8_t st,cool,t,flash,dodge,face,crouch,want;} Dark;
static Dark da PCE_WORK;
extern Body *phys_body;
void phys_call(void);
extern uint16_t rng;
extern uint8_t boss_max;
extern const Actor *fire_actor;
extern uint8_t fire_aim;
extern int8_t fire_mx,fire_my;
extern bool fire_fast;
void fire_call(void);
__attribute__((noinline,minsize,section(".ram_bank115.text"))) void dark_begin(void) {
    memset(&da,0,sizeof da);
    da.b.x=player.x<(int16_t)camera+128?(int16_t)camera+200:(int16_t)camera+56;da.b.y=player.y;   /* on the far side from the hero */
    pce_campaign.boss_hp=boss_max=pce_options.difficulty==0?14:pce_options.difficulty==1?30:40;
    audio_stop();
}
/* The buttons of this step (da.want). */
DARK_AI static void think(void) {
    uint8_t carry=rng&1,r,w;
    rng>>=1;if(carry)rng^=0xB400;
    r=(uint8_t)(rng^(rng>>8));
    int16_t dx=player.x-da.b.x,adx=dx<0?-dx:dx;
    uint8_t toward=dx>0?KEY_RIGHT:KEY_LEFT,away=dx>0?KEY_LEFT:KEY_RIGHT;
    bool ground=da.b.coll&4;
    if(da.dodge){--da.dodge;da.want=KEY_2|away;return;}
    if(ground&&(pce_control.keys&KEY_1)&&r<50)da.dodge=14;   /* the hero's fire: hop back from it */
    if(adx<110)w=away;
    else if(adx>190)w=toward|(r<150?KEY_1:0);
    else {w=KEY_SELECT|KEY_1;if(player.y+8<da.b.y-24)w|=KEY_UP;else if(player.y>da.b.y+24)w|=KEY_DOWN;}
    if(((w&KEY_LEFT)&&da.b.x<(int16_t)camera+24)||((w&KEY_RIGHT)&&da.b.x>(int16_t)camera+232))w=(w&~(KEY_LEFT|KEY_RIGHT))|(dx>0?KEY_RIGHT:KEY_LEFT)|KEY_2;   /* backed into a wall: hop out toward the hero */
    da.want=w;
}
/* Her shot, and the hero's shots and body against hers. */
DARK_HIT static void hits(void) {
    uint8_t w=da.want;
    if((w&KEY_1)&&!da.cool) {   /* aimed level, or 45 degrees up or down (fire_call: 0 L, 1 UL, 3 UR, 4 R, 5 DR, 7 DL) */
        uint8_t up=w&KEY_UP,down=(w&KEY_DOWN)&&!da.crouch;
        fire_actor=(const Actor*)&da;fire_aim=da.face?(up?1:down?7:0):(up?3:down?5:4);fire_mx=da.face?-22:22;fire_my=da.crouch?8:-1;fire_fast=true;
        overlay_call(0x69,fire_call);
        da.cool=pce_campaign.boss_hp*2<=boss_max?18:25;
    }
    for(Shot *s=shots;s<shots+NSHOTS;++s) {
        int16_t dy=s->y-da.b.y;
        if(!s->active||s->enemy||(uint16_t)(s->x-da.b.x+12)>=24||dy<(da.crouch?-10:-22)||dy>=22)continue;   /* a crouch ducks a level shot */
        s->active=0;da.flash=6;audio_effect(7);
        if(pce_campaign.boss_hp>1)--pce_campaign.boss_hp;
        else{da.st=D_DYING;da.t=0;audio_effect(8);}
    }
    if(!safe_timer&&(uint16_t)(player.x-da.b.x+14)<28&&(uint16_t)(player.y-da.b.y+28)<56){safe_timer=120;campaign_hurt();}   /* touching her hurts the hero */
}
/* The scenes and their timers (in $7b). */
__attribute__((noinline,minsize,section(".ram_bank123.text"))) static void scenes(void) {
    if(da.flash)--da.flash;if(da.cool)--da.cool;
    da.want=0;
    switch(da.st) {
    case D_WAIT:if(++da.t>=108){pce_campaign.story=hero==2?4:1;pce_campaign.event=1;da.st=D_CALL;da.t=0;}break;   /* the radio call */
    case D_APPEAR:if(++da.t>=108){pce_campaign.story=hero==2?5:2;pce_campaign.event=1;da.st=D_MEET;da.t=0;}break;
    case D_CALL:case D_MEET:da.st++;da.t=0;if(da.st==D_READY)pce_metrics.hp=campaign_hearts();break;   /* (a scene has closed) the call, then she forms; the meeting, then Ramrod tops the hero up */
    case D_READY:if(++da.t>=54){da.st=D_FIGHT;audio_music(8);}break;
    case D_FIGHT:overlay_call(0x6e,think);break;
    default:if(++da.t>=162)pce_campaign.boss_hp=0;
    }
}
DARK_CODE void dark_tick(void) {
    overlay_call(0x7b,scenes);
    if(!pce_campaign.boss_hp)return;
    uint8_t w=da.want;
    /* the body, with the hero's own controls */
    bool ground=da.b.coll&4;
    da.crouch=da.st==D_FIGHT&&ground&&(w&KEY_DOWN)&&!(w&(KEY_LEFT|KEY_RIGHT|KEY_SELECT));
    da.b.vx=0;
    if(da.st==D_FIGHT&&!(w&KEY_SELECT)&&!da.crouch){if(w&KEY_LEFT)da.b.vx=-427;else if(w&KEY_RIGHT)da.b.vx=427;}
    da.face=da.b.vx?da.b.vx<0:player.x<da.b.x;   /* she faces the hero unless she is walking away from him */
    if((w&KEY_2)&&ground)da.b.vy=-1237;
    phys_body=&da.b;overlay_call(0x69,phys_call);
    if(da.st==D_FIGHT)overlay_call(0x70,hits);
}
DARK_HIT void dark_draw(void) {
    uint8_t t=da.t;
    if(da.st<=D_CALL||((da.flash||da.st==D_APPEAR||(da.st==D_DYING&&t>66))&&(frame&2)))return;   /* not yet formed; flickering in, hit, and coming apart */
    uint16_t id=pce_dark_base[pce_metrics.stage-1];
    if(da.st==D_DYING)id+=14+(t<48?t/16:3);
    else if(!(da.b.coll&4))id+=1;
    else if(da.crouch)id+=7;
    else if(da.cool>10)id+=8;                                          /* a shot has just left */
    else if(da.b.vx)id+=1+(frame>>2)%6;
    video_sprite_optional(id,da.b.x-camera,da.b.y-16,da.face,16);
}
