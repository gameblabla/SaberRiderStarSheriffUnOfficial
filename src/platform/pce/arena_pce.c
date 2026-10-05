#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
/* Stage 4's finale (forest.c forest_finale_update): the Outriders run in from both edges for 24 s while the radio scene's music plays, then the
 * Hyperjumper comes back while fewer of them keep coming. The source's three edge streams each have their own wait: gunmen from the right
 * (2.1 s), chargers from both sides (2.5 s), blue stalkers from both sides (3.0 s); the boss round's are thinner (3.4 s / 4.6 s / 5.2 s). The stalkers
 * are the blue grunts here (they walk in and fire; the standing snipers would wait off screen). Bodies on the field are capped (the source's 8 / 5;
 * the actor pool and the scanline budget give 6 / 4) and nothing comes while sprites are being refused (priority_pce.c). Called every fourth step,
 * so the waits are in units of 4 steps. $76: the CD buffer's bank (the code is read back after every load, pce_config.h PCE_X1). */
static uint8_t arena_wait[3] PCE_WORK;
static uint8_t arena_flip PCE_WORK;
PCE_X1 void arena_spawn(void) {
    static const uint8_t every[2][3]={{32,37,45},{51,69,78}};
    static const uint8_t kind[2][3]={{2,1,5},{1,2,5}};
    static const uint8_t side[2][3]={{0,2,2},{0,1,0}};   /* 0 from the right, 1 from the left, 2 both in turn */
    bool boss=pce_campaign.boss_kind!=0;
    uint8_t live=0;
    for(const Actor *a=actors;a<actors+8;++a)if(a->active&&!a->dead&&a->type<11)++live;
    for(uint8_t k=0;k<3;++k) {
        if(arena_wait[k]){--arena_wait[k];continue;}
        if(live>=(boss?4:6)||enemy_pressure){arena_wait[k]=4;continue;}
        Actor *a=actors;while(a<actors+8&&a->active)++a;
        if(a==actors+8)break;
        bool left=side[boss][k]==1||(side[boss][k]==2&&(arena_flip^=1));
        *a=(Actor){.b={.x=left?camera-24:camera+280,.y=170},.active=1,.type=kind[boss][k],.hp=1,.flip=!left,.aim=4,.mode=1};
        ++live;
        arena_wait[k]=every[boss][k];
    }
}
/* the finale begins: the first wait of each stream (0.3 s, 1.4 s, 1.9 s) */
PCE_X1 void arena_reset(void) {arena_wait[0]=5;arena_wait[1]=21;arena_wait[2]=29;arena_flip=0;}
