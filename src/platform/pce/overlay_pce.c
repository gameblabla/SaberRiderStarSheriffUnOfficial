#include "overlay_pce.h"
#include "play_pce.h"
#include "campaign_pce.h"
PceControl pce_control;
__attribute__((noinline,callback(2))) void overlay_call(uint8_t bank,void (*method)(void)) {
    uint8_t previous=pce_bank3_get();
    pce_bank3_set(bank);
    method();
    pce_bank3_set(previous);
}
/* The palette snapshot (ui_fade(8)) whitened by ui_lift_level steps: the held frame whitening out before a victory painting. */
extern uint8_t buffer[2048];
uint8_t ui_lift_level;
__attribute__((noinline,section(".ram_bank117.text"))) void ui_lift(void) {
    uint16_t *src=(uint16_t*)buffer,*dst=src+512;
    for(uint16_t i=0;i<512;++i) {
        uint16_t c=src[i];uint8_t b=c&7,r=(c>>3)&7,g=(c>>6)&7;
        b=b+ui_lift_level>7?7:b+ui_lift_level;r=r+ui_lift_level>7?7:r+ui_lift_level;g=g+ui_lift_level>7?7:g+ui_lift_level;
        dst[i]=(uint16_t)g<<6|r<<3|b;
    }
    pce_vce_copy_palette(0,dst,32);
}
PCE_CODE void play_start(void) { play_init(pce_control.stage,pce_control.hero); }
PCE_CODE void play_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&(pce_campaign.diagnostic||pce_campaign.state==CAM_PLAY)&&!pce_campaign.event&&!pce_campaign.result;++i) {
        play_tick(pce_control.keys,i?0:pce_control.pressed);pce_control.pressed=0;
    }
    overlay_call(0x7b,play_draw);
}
PCE_CODE void play_present(void) { overlay_call(0x7b,play_draw); }
int main(void) {
    pce_bank3_set(0x6e);
    flow_main();
    for(;;){}
}
