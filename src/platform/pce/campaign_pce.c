#include "campaign_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
PceCampaign pce_campaign={.lives=3,.powers=2};
PceOptions pce_options={.difficulty=1,.lives=3,.continues=3,.music=3};
uint8_t pce_continues=3;
uint8_t campaign_hearts(void) { return pce_options.difficulty==0?3:pce_options.difficulty==1?2:1; }
void campaign_hurt(void) {
    audio_effect(5);
    if(pce_metrics.hp)--pce_metrics.hp;
    if(!pce_metrics.hp) {
        audio_effect(6);
        if(pce_campaign.lives) { --pce_campaign.lives;pce_campaign.result=2; }
        else {pce_campaign.state=CAM_OVER;pce_campaign.timer=0;}
    }
}
