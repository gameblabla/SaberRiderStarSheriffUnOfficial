#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
/* Level triggers: a hero inside a trigger's zone spawns its enemy. Lives in the combat bank to leave the platform
 * bank room. Positions retain whole-world range (see play_pce.c). */
typedef struct __attribute__((packed)) {
    int16_t cx,cy,hx,hy; uint16_t interval,delay,type;
    uint8_t layer; int8_t remaining; uint8_t nwp; int16_t wp[8][2];
} Trigger;
_Static_assert(sizeof(Trigger)==49,"Trigger format changed");
static uint16_t trigger_timers[100];
static int8_t trigger_remaining[100];
static uint8_t trigger_spawned[100];
static Trigger trigger;
static Trigger trigger_cache[60] PCE_STAGE;
static int16_t trigger_lo[60] PCE_STAGE,trigger_hi[60] PCE_STAGE;
PCE_COMBAT void encounter_init(void) {
    const PceScene *scene=play_scene;
    for(uint8_t k=0;k<scene->ntr;++k) {
        arcade_read(2,scene->triggers+(uint32_t)k*sizeof trigger,&trigger,sizeof trigger);
        if(k<60) {trigger_cache[k]=trigger;trigger_lo[k]=trigger.type==10?32767:trigger.cx-trigger.hx-8;trigger_hi[k]=trigger.cx+trigger.hx+8;}
        trigger_timers[k]=trigger.delay;trigger_remaining[k]=trigger.remaining;trigger_spawned[k]=0;
    }
}
PCE_COMBAT void encounters(void) {
    const PceScene *scene=play_scene;
    uint8_t k=0;
    int16_t px=player.x+4;
    for(const Trigger *t=trigger_cache;k<scene->ntr;++k,++t) {
        if(px<trigger_lo[k]||px>trigger_hi[k]||!trigger_remaining[k]) continue;
        if(player.y+9<t->cy-t->hy-23||player.y+9>t->cy+t->hy+23) continue;
        if(trigger_timers[k]) {trigger_timers[k]=trigger_timers[k]>4?trigger_timers[k]-4:0;continue;}
        if(t->type==11) {
            /* The robot-horse herd: the source drops a column of 12 horses at once, 99 px apart, behind the screen
             * edge; here five (the SAT holds two on screen) 132 px apart, running at the hero. */
            int16_t x=t->wp[0][0];bool right=x>30000;
            int16_t x0=right?camera+256+32+56+195:camera-32-56-195;
            for(uint8_t h=0,i=0;h<5;++h) {
                while(i<8&&actors[i].active)++i;
                if(i==8)break;
                actors[i]=(Actor){.b={.x=right?x0+h*132:x0-h*132,.y=t->wp[0][1]+30},.active=1,.type=11,.hp=1,.flip=right};
            }
            trigger_remaining[k]=0;continue;
        }
        /* Humanoid core first; the inventory retains other encounter recipes
         * for their stage-specific handlers rather than replacing their art. */
        for(uint8_t i=0;i<8;++i) if(!actors[i].active) {
            uint8_t wp=trigger_spawned[k]%t->nwp;
            int16_t x=t->wp[wp][0],y=t->wp[wp][1];
            bool edge=x>30000||x< -30000;
            if(x>30000)x=camera+288;else if(x< -30000)x=camera-32;
            if(y>30000)y=256;else if(y< -30000)y=-32;
            if(y< -999)y=-1000-y;
            if(edge&&t->type<6){probe_x=x+8;probe_y=y+19;probe_left=player.x<x;overlay_call(0x69,spawn_clear);y=probe_y-19;}
            actors[i]=(Actor){.b={.x=t->type>=11&&t->type<=27?x:x+8,.y=t->type>=11&&t->type<=27?y:y+19},.active=1,.type=t->type,.hp=t->type>=30?6:2,.timer=t->type<6?36:60,.flip=player.x<x};
            if(trigger_remaining[k]>0)--trigger_remaining[k];
            ++trigger_spawned[k];trigger_timers[k]=t->interval;break;
        }
    }
}
