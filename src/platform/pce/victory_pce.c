#include "frontend_pce.h"
#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "campaign_pce.h"
#define VICTORY_CODE __attribute__((noinline,section(".ram_bank114.text"),minsize))
extern uint8_t buffer[2048];
extern uint8_t previous;

/* A stage is cleared. The stage's jingle (CD-DA track 6, started once by the caller) plays over the held last frame, the
 * picture whitens as in the main game, and the stage's painting follows: the centred 320x224 of the original with
 * MISSION ACCOMPLISHED as opaque sprites on top (frontend.py victory_screen) under
 * the victory music (track 7). Any button leaves it. The painting is read only after the jingle (a disc read stops CD-DA). */
VICTORY_CODE void frontend_victory(void) {
    previous=0;
    ui_fade(8);
    uint8_t lift=0;
    for(uint16_t t=0;t<360;++t) {
        video_wait();ui_read_keys();
        if(t>60&&(ui_pressed&(KEY_RUN|KEY_1|KEY_2)))break;
        if(t>=300&&!(t&7)&&lift<7){ui_lift_level=++lift;overlay_call(0x72,ui_lift);}
    }
    audio_stop();
    static const uint8_t stage_offset[7]={0,4,5,9,13,17,18};
    uint8_t stage=pce_metrics.stage;
    uint8_t index=stage_offset[stage-1]+((stage==2||stage>=6)?0:pce_control.hero);
    uint8_t id=PCE_UI_VICTORY_BASE+index;
    if(!loader_victory(index))return;
    ui_dark=1;ui_show(id);ui_dark=0;
    audio_music_once(7);
    pce_ui_state=5;   /* (the BIOS call can take a while: input is read from here on) */
    /* The lettering: opaque 16x16 sprites over the painting (frontend.py victory_screen), from the screen's extra blob. */
    static uint16_t pieces[1+32*4];
    uint32_t table=pce_ui[id].extra+PCE_UI_RAMP_BYTES;
    arcade_read(2,table,pieces,sizeof pieces);
    uint8_t count=pieces[0];
    video_sat_begin();
    for(uint8_t k=0;k<count;++k)ui_sprite(pieces[1+k*4],pieces[2+k*4],pieces[3+k*4],0,false);
    video_sat_end();
    ui_black();ui_fade_in();   /* the painting comes up from black (the common fade) */
    /* A button leaves it; so does the clock: the jingle's 6 s and this painting's 4.5 s make 11 s (with the fades, a little over), then the fade to black. */
    for(uint16_t t=0;t<270;++t) {
        video_wait();ui_read_keys();
        if(t>60&&(ui_pressed&(KEY_RUN|KEY_1|KEY_2)))break;
    }
    ui_fade_out();
    ui_end();audio_stop();pce_ui_state=0;previous=ui_held;
}
