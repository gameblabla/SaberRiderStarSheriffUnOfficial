#include "frontend_pce.h"
#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "campaign_pce.h"
#define VICTORY_CODE __attribute__((noinline,section(".ram_bank114.text")))
extern uint8_t buffer[2048];
extern uint8_t previous;

/* A stage is cleared. The stage's jingle (CD-DA track 6, started once by the caller) plays over the held last frame, the
 * picture whitens as in the main game, and the stage's painting follows: the centred 320x224 of the original with
 * MISSION ACCOMPLISHED added as pulsing light (frontend.py glow_screen, the same bit-plane trick as GAME OVER) under
 * the victory music (track 7). Any button leaves it. The painting is read only after the jingle (a disc read stops CD-DA). */
VICTORY_CODE void frontend_victory(void) {
    previous=0;
    ui_fade(8);
    uint8_t lift=0;
    for(uint16_t t=0;t<360;++t) {
        video_wait();ui_read_keys();
        if(t>60&&(ui_pressed&(KEY_RUN|KEY_1|KEY_2)))break;
        if(t>=300&&!(t&7)&&lift<7){ui_lift_level=++lift;overlay_call(0x69,ui_lift);}
    }
    audio_stop();
    static const uint8_t stage_offset[7]={0,4,5,9,13,17,18};
    uint8_t stage=pce_metrics.stage;
    uint8_t index=stage_offset[stage-1]+((stage==2||stage>=6)?0:pce_control.hero);
    uint8_t id=PCE_UI_VICTORY_BASE+index;
    if(!loader_victory(index))return;
    ui_dark=1;ui_show(id);ui_dark=0;
    ui_fade(8);ui_fade(7);video_display(true);
    audio_music_once(7);
    pce_ui_state=5;   /* (the BIOS call can take a while: input is read from here on) */
    static const uint8_t pulse[8]={0,1,2,3,3,2,1,0};
    uint32_t glow=pce_ui[id].extra+PCE_UI_RAMP_BYTES;
    uint8_t level=7,shown=0;bool leaving=false;
    for(uint16_t t=0;;++t) {
        video_wait();ui_read_keys();
        if(!level&&!leaving&&!(t&7)&&pulse[(t>>3)&7]!=shown) {
            shown=pulse[(t>>3)&7];
            arcade_read(2,glow+(uint32_t)shown*PCE_UI_GAMEOVER_SLOTS*32,buffer,PCE_UI_GAMEOVER_SLOTS*32);
            pce_vce_copy_palette(PCE_UI_GAMEOVER_SLOT,buffer,PCE_UI_GAMEOVER_SLOTS);
        }
        if(!(t%3)) {
            if(!leaving&&level){ui_fade(--level);}
            else if(leaving){ui_fade(++level);if(level==7)break;}
        }
        if(!leaving&&t>60&&(ui_pressed&(KEY_RUN|KEY_1|KEY_2))){leaving=true;ui_fade(8);level=0;}
    }
    ui_end();audio_stop();pce_ui_state=0;previous=ui_held;
}
