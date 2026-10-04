#include "overlay_pce.h"
#include "play_pce.h"
#include "floor_pce.h"
#include "campaign_pce.h"
PceControl pce_control;
__attribute__((noinline,callback(2))) void overlay_call(uint8_t bank,void (*method)(void)) {
    uint8_t previous=pce_bank3_get();
    pce_bank3_set(bank);
    method();
    pce_bank3_set(previous);
}
PCE_CODE void play_start(void) { play_init(pce_control.stage,pce_control.hero); }
PCE_CODE void play_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&(pce_campaign.diagnostic||pce_campaign.state==CAM_PLAY)&&!pce_campaign.event&&!pce_campaign.result;++i) {
        play_tick(pce_control.keys,i?0:pce_control.pressed);pce_control.pressed=0;
    }
    overlay_call(0x7b,play_draw);
}
PCE_CODE void play_present(void) { overlay_call(0x7b,play_draw); }
PCE_FLOOR void floor_start(void) { pce_control.ok=floor_init(); }
PCE_FLOOR void floor_draw(void) {
    floor_update(pce_control.x,pce_control.y,pce_control.heading,pce_control.phase);
    if(pce_campaign.diagnostic)floor_present();
}
int main(void) {
    pce_bank3_set(0x6e);
    flow_main();
    for(;;){}
}
