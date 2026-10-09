#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "scenery_pce.h"
#include "herd_geometry.h"
/* Level triggers: a hero inside a trigger's zone spawns its enemy. Lives in the combat bank to leave the platform
 * bank room. Positions retain whole-world range (see play_pce.c). */
typedef struct __attribute__((packed)) {
    int16_t cx,cy,hx,hy; uint16_t interval,delay,type;
    uint8_t layer; int8_t remaining; uint8_t nwp; int16_t wp[8][2];
} Trigger;
_Static_assert(sizeof(Trigger)==49,"Trigger format changed");
extern uint16_t rng;   /* play_pce.c */
static uint16_t trigger_timers[100];
static int8_t trigger_remaining[100];
static uint8_t trigger_spawned[100];
static Trigger trigger;
static int16_t stop_zones[4][4];
static uint8_t stop_count,stop_done;
static uint8_t convoy_triggers[4],convoy_count;
Trigger trigger_cache[60] PCE_STAGE;   /* (Ramrod's arena uses this staging memory for its own state: mech_pce.h) */
static int16_t trigger_lo[60] PCE_STAGE,trigger_hi[60] PCE_STAGE;
PCE_CODE void encounter_init(void) {
    const PceScene *scene=play_scene;
    convoy_count=0;
    uint8_t counts[3];arcade_read(2,scene->rules,counts,3);
    stop_count=counts[2]>4?4:counts[2];stop_done=0;herd_on=herd_locked=herd_pending=herd_flee=0;
    arcade_read(2,scene->rules+3+(uint16_t)counts[0]*16+(uint16_t)counts[1]*12,stop_zones,stop_count*8);
    for(uint8_t k=0;k<scene->ntr;++k) {
        arcade_read(2,scene->triggers+(uint32_t)k*sizeof trigger,&trigger,sizeof trigger);
        if(k<60) {trigger_cache[k]=trigger;trigger_lo[k]=trigger.type==10?32767:trigger.cx-trigger.hx-8;trigger_hi[k]=trigger.cx+trigger.hx+8;}
        if(trigger.type==11) {
            if(convoy_count<4)convoy_triggers[convoy_count++]=k;
            else convoy_count=255; /* Future scenes with more convoys use the full scan. */
        }
        trigger_timers[k]=trigger.delay;trigger_remaining[k]=trigger.remaining;trigger_spawned[k]=0;
        if(trigger.type>=24&&trigger.type<=27)trigger_remaining[k]=0;   /* background airships: not drawn here, so never spawned */
    }
}
/* The convoy is instantiated lazily: the source drops all of its horses at once (12-13, 99 px apart).
 * The PCE spaces them 224 px apart to fit the real scanline limit. The actor pool holds eight, so each horse is created when its place in the column comes within a few horses of the screen. The
 * column marches at the horses' own speed (2 px a step, 8 per call), so `herd_next` is where the next one would be. */
uint8_t herd_pending,herd_flee;int16_t herd_next;
PCE_SCENERY void herd_feed(void) {
    herd_next-=8;
    while(herd_pending&&herd_next<(int16_t)camera+328+(int16_t)herd_lead+200) {
        uint8_t i=0;while(i<8&&actors[i].active)++i;
        if(i==8)break;
        actors[i]=(Actor){.b={.x=herd_next,.y=(((herd_y+30-48)+4)&~7)+48},.active=1,.type=11,.hp=1,.flip=1};
        herd_next+=HERD_SPACING;--herd_pending;
    }
}
PCE_SCENERY void encounters(void)   /* $78: the mission bank is full */ {
    if(herd_pending)herd_feed();
    const PceScene *scene=play_scene;
    uint8_t k=0;
    int16_t px=player.x+4;
    if(herd_on)for(uint8_t j=0;j<stop_count;++j) {
        const int16_t *z=stop_zones[j];
        if(!(stop_done&(1<<j))&&px>=z[0]-z[2]-8&&px<=z[0]+z[2]+8&&
           player.y+9>=z[1]-z[3]-23&&player.y+9<=z[1]+z[3]+23) {
            stop_done|=1<<j;herd_locked=1;
        }
    }
    /* Nothing else is brought in as a convoy is about to start (its zone is within a screen): the stampede runs through
     * a cleared stretch, so no enemy is drawn, cached or fired from while its sprite budget is spoken for. */
    bool convoy_near=herd_on;
    if(scene->horse&&!convoy_near) {
        if(convoy_count==255) {
            for(const Trigger *t=trigger_cache;k<scene->ntr;++k,++t)
                if(t->type==11&&trigger_remaining[k]&&px>trigger_lo[k]-160&&px<trigger_hi[k]+160){convoy_near=true;break;}
        } else for(uint8_t j=0;j<convoy_count;++j) {
            uint8_t n=convoy_triggers[j];
            if(trigger_remaining[n]&&px>trigger_lo[n]-160&&px<trigger_hi[n]+160){convoy_near=true;break;}
        }
    }
    k=0;
    for(const Trigger *t=trigger_cache;k<scene->ntr;++k,++t) {
        if(!trigger_remaining[k]||px<trigger_lo[k]||px>trigger_hi[k]) continue;
        if(player.y+9<t->cy-t->hy-23||player.y+9>t->cy+t->hy+23) continue;
        if(trigger_timers[k]) {trigger_timers[k]=trigger_timers[k]>4?trigger_timers[k]-4:0;continue;}
        if(t->type==11) {
            /* The herd begins before the separately exported camera-stop zone. The source drops a column
             * of 12 horses at once, 99 px apart, behind the right screen edge, running at the hero at 120 px/s; here
             * the same horse count, 224 px apart, so at most two are on screen and two fit a scanline beside the hero. */
            if(!play_scene->horse||t->wp[0][0]<=30000){trigger_remaining[k]=0;continue;}
            herd_y=t->wp[0][1];herd_lead=t->interval*5/2;herd_pending=(uint8_t)t->remaining+1;if(pce_metrics.stage==1)herd_pending=t->cx<4000?8:t->cx>8000?herd_pending-3:herd_pending;   /* the first convoy runs a third shorter, the last three horses fewer */
            herd_flee=1;   /* every stampede: its enemies turn and run off to the left (not just the first of level 1) */
            herd_next=camera+328+herd_lead+16;overlay_call(0x6f,herd_spawn);herd_feed();
            trigger_remaining[k]=0;continue;
        }
        /* Nothing else is called in while the herd runs, and what a zone would have brought in meanwhile is gone: in the source it would have
         * stood in the horses' way and been trampled, so a placed enemy must not walk in once the stampede is over. */
        if(herd_on){if(trigger_remaining[k]>0)trigger_remaining[k]=0;continue;}
        /* Placed enemies always come when their zone is crossed; the endless streams use the room that is left (priority_pce.c). */
        if(t->type!=28&&(t->type<11||t->type>28)) {
            if(convoy_near)continue;   /* (a convoy is about to start) */
            pce_control.y=trigger_remaining[k]>0;overlay_call(0x76,spawn_room);
            if(!pce_control.x)continue;
        }
        /* Humanoid core first; the inventory retains other encounter recipes
         * for their stage-specific handlers rather than replacing their art. */
        for(uint8_t i=0;i<8;++i) if(!actors[i].active) {
            uint8_t wp=trigger_spawned[k]%t->nwp;
            int16_t x=t->wp[wp][0],y=t->wp[wp][1];
            bool edge=x>30000||x< -30000,drop_in=y< -999;   /* a spawn point below -999 is a drop-in: the enemy is thrown up out of it */
            if(x>30000)x=camera+288;else if(x< -30000)x=camera-32;
            if(y>30000)y=256;else if(y< -30000)y=-32;
            if(y< -999)y=-1000-y;
            if(edge&&t->type<6){probe_x=x+8;probe_y=y+19;probe_left=player.x<x;overlay_call(0x81,spawn_clear);y=probe_y-19;}
            actors[i]=(Actor){.b={.x=t->type>=11&&t->type<=27?x:x+8,.y=t->type>=11&&t->type<=27?y:y+19},.active=1,.type=t->type,.hp=t->type==28?2:t->type>=30?(pce_options.difficulty==0?4:pce_options.difficulty==1?6:8):1,.timer=t->type>=30&&t->type<=31?60:0,.flip=player.x<x,.aim=4,.mode=1};
            if(drop_in){actors[i].mode|=8;actors[i].b.vy=-711;}   /* 166.7 px/s, in Q8 a step */
            if(trigger_remaining[k]>0)--trigger_remaining[k];
            ++trigger_spawned[k];
            /* the source adds up to rand_n * 0.02 s to every wait (exported in the trigger's layer byte) */
            trigger_timers[k]=t->interval+(t->layer?(uint16_t)((uint8_t)(rng>>4)%(t->layer+1))*6/5:0);break;
        }
    }
}
