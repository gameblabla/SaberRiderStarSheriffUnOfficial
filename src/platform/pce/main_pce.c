#include "video_pce.h"
#include "loader_pce.h"
#include "sgx_pce.h"
#include "arcade_pce.h"
#include "road_pce.h"
#include "play_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#include "audio_pcm.h"
#include "presentation_pce.h"
#include "frontend_pce.h"
#include "ui_pce.h"
#ifndef PCE_START_STAGE
#define PCE_START_STAGE 1
#endif
uint8_t previous;
static uint8_t stage, hero;
#ifndef RETAIL
static uint8_t selected, menu, heading, phase;
static uint8_t menu_hero,menu_mode;
static uint16_t race_x=4696,race_y=4096;
#else
static uint8_t paused;
void pause_show(void),pause_hide(void);
#endif
static uint8_t simulation_tick,resume_phase;   /* resume_phase: the race phase / arena wave a restart after a lost life begins in (change_stage) */
void race_briefing_frame(void);
#ifndef RETAIL
static const int8_t sine[32] PCE_TABLE={0,25,49,71,90,106,117,125,127,125,117,106,90,71,49,25,0,-25,-49,-71,-90,-106,-117,-125,-127,-125,-117,-106,-90,-71,-49,-25};
#endif
PCE_FLOW static bool change_stage(uint8_t n) {
    uint8_t resume=resume_phase;resume_phase=0;
    /* Every card owns the standard font. Race dialogue replaces it with
     * double-width glyphs, and front-end screens can replace it too. Reload
     * only after hiding the old screen, before the card publishes any text. */
    audio_stop();video_display(false);pce_raster_enabled=0;
    if(!loader_font())return false;
    pce_control.stage=n;overlay_call(0x76,frontend_card);
    pce_metrics.ready=0;stage=n;
#ifndef RETAIL
    phase=heading=0;race_x=4696;race_y=4096;
#endif
    pce_metrics.phase=0;pce_metrics.camera_x=pce_metrics.hp=0;
    if(!loader_scene(stage))return false;
    if(stage==6)m6_load();   /* Ramrod's arena's code images, into the banks the scene load has just put back */
    if(!loader_voice(hero))return false;
    pce_control.stage=stage;pce_control.hero=hero;pce_control.phase=resume;
    pce_campaign.state=CAM_PLAY;pce_campaign.result=pce_campaign.event=0;pce_campaign.boss_kind=0;
    if(stage==2) {
        overlay_call(0x6d,road_start);if(!pce_control.ok)return false;
        video_sat_begin();video_sprite(5,256,215,false,16);
        video_sprite(9,180,177,false,16);video_sprite(12,360,163,true,16);
        video_sat_end();
    }
    else {
        if(!video_background(stage==6?385:0))return false;   /* (Ramrod's arena opens facing the planet)*/
        if(stage!=6&&stage!=7)overlay_call(0x69,play_start);
    }
    if(!pce_campaign.diagnostic) {
        if(stage==2){
            overlay_call(0x79,race_start);pce_control.elapsed=0;
            /* The briefing freezes the world. Draw the first road before
             * opening it. */
            overlay_call(0x7b,race_briefing_frame);
        }
        else if(stage==6)overlay_call(0x79,M6_START);
        else if(stage==7)overlay_call(0x73,space_start);
        if((stage==2||stage==6)&&!resume) {pce_campaign.story=0;overlay_call(0x71,story_start);}   /* (a restart in the pursuit or a later wave goes straight on) */
    }
    audio_music(stage==1?5:stage==2?(resume?14:10):stage==3?13:stage==4?15:stage==5?14:stage==6?16:12);
    ui_black();ui_fade_in();   /* every stage comes up from black (the common transition) */
    pce_metrics.ready=1;return true;
}
#ifndef RETAIL
PCE_FLOW static void menu_draw(void) {
    video_text(1,2,"SABER RIDER");
    video_text(1,4,"ARCADE CD-ROM2");
    video_text(1,6,"LEFT/RIGHT STAGE");video_number(23,6,selected);
    video_text(1,8,"UP/DOWN HERO");video_number(23,8,hero+1);
    video_text(1,10,"RUN START");
    video_text(1,12,pce_campaign.diagnostic?"II MODE: DIAGNOSTICS":"II MODE: CAMPAIGN   ");
}
PCE_FLOW static void render_test(uint8_t keys,uint8_t pressed) {
    if(stage==2) {
        if(keys&KEY_LEFT)heading=(heading-1)&31;
        if(keys&KEY_RIGHT)heading=(heading+1)&31;
        if(keys&KEY_UP) {
            race_x=(race_x+(sine[(heading+8)&31]>>5))&8191;
            race_y=(race_y+(sine[heading]>>5))&8191;
        }
        if(pressed&KEY_SELECT) {
            phase^=1;if(phase){race_x=4096;heading=8;}
        }
        pce_control.x=race_x;pce_control.y=race_y;pce_control.heading=heading<<2;pce_control.phase=phase;
        overlay_call(0x6d,road_draw);
    }
    pce_metrics.phase=phase;pce_metrics.player_x=race_x;pce_metrics.player_y=race_y;
}
#endif
/* The pad. A 6-button pad (in its 6-button mode) answers alternate reads with its extra buttons: the direction nibble reads 0000 (all four
 * pressed, which no D-pad can do) and the buttons nibble holds III, IV, V, VI. Two reads a frame return both sets, in either order. Button III is
 * the hero power. On a 2-button pad the power is a tap of Select (pressed and let go within 20 frames with no direction held meanwhile: Select
 * held with a direction is the aim). */
static uint8_t pad_six,select_clock,select_dirty;
PCE_FLOW static uint8_t read_pad(uint8_t *pressed_power) {
    uint8_t a=~pce_joypad_read(),b=~pce_joypad_read(),keys=a,third=0;
    if((a&0xf0)==0xf0){third=a&1;keys=b;pad_six=1;}
    else if((b&0xf0)==0xf0){third=b&1;pad_six=1;}
    static uint8_t third_before,select_before;
    *pressed_power=0;
    if(pad_six){if(third&&!third_before)*pressed_power=1;}
    else {
        if(keys&KEY_SELECT){if(!select_before){select_clock=0;select_dirty=0;}if(select_clock<255)++select_clock;if(keys&(KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))select_dirty=1;}
        else if(select_before&&!select_dirty&&select_clock<=20)*pressed_power=1;
    }
    third_before=third;select_before=keys&KEY_SELECT;
    if(pce_options.swap_buttons) {
        uint8_t buttons=keys&(KEY_1|KEY_2);
        if(buttons==KEY_1||buttons==KEY_2)keys^=KEY_1|KEY_2;
    }
    return keys;
}
PCE_FLOW void flow_main(void) {
    pce_metrics.magic[0]='S';pce_metrics.magic[1]='R';pce_metrics.magic[2]='P';pce_metrics.magic[3]='C';
    pce_metrics.version=1;video_init();audio_pcm_init();
    if(!loader_font())for(;;){}
#ifdef PCE_SGX
    {
        uint8_t observed=pce_ticks;
        while(observed==pce_ticks) {}
        overlay_call(0x78,pce_sgx_detect_init);
        /* On ordinary PCE hardware, pce_sgx_active stays clear and the UI
           loader uses the original one-VDC records below. */
    }
#endif
    if((pce_cdb_version()>>8)<3||!arcade_detect()) {
        video_vdc(VDC_REG_MEMORY,VDC_BG_SIZE_64_32);
        video_vdc(0,0);
        for(uint16_t i=0;i<2048;++i)video_vdc(2,0xf000|(PCE_FONT_WORD>>4));
        pce_vce_set_color(0,0);pce_vce_set_color(255,0x1ff);
        video_sat_begin();video_sat_end();
        video_text(1,10,"SUPER CD V3 + ARCADE CARD");video_text(1,12,"REQUIRED");video_display(true);
        for(;;)video_wait();
    }
    pce_metrics.arcade_ports=arcade_selftest();if(pce_metrics.arcade_ports!=15)for(;;){}
    pce_control.stage=0;pce_control.hero=hero;
    overlay_call(0x72,frontend_start);if(!pce_control.ok)for(;;){};
    hero=pce_control.hero;
    uint8_t initial=pce_control.stage?pce_control.stage:PCE_START_STAGE;
    if(!change_stage(initial))for(;;){}
    simulation_tick=pce_ticks;
    for(;;) {
        /* The race runs as fast as it can (a pass is rarely a whole number of frames, and waiting out the rest wastes the
         * time the floor wants): the wait is only for the first tick after the last pass began. */
        if(stage==2){while(pce_ticks==simulation_tick){}++pce_metrics.frames;}else video_wait();
        uint8_t elapsed=pce_ticks-simulation_tick;simulation_tick=pce_ticks;
        if(elapsed>12)elapsed=12;
        if(pce_stall){pce_stall=0;elapsed=1;}   /* a CD seek or a big upload held the loop up: its ticks are not caught up */
        uint8_t power,keys=read_pad(&power),pressed=keys&~previous;previous=keys;audio_tick();
#ifdef RETAIL
        if((pressed&KEY_RUN)&&(paused||pce_campaign.state==CAM_PLAY)) {
            paused^=1;
#ifdef PCE_SGX
            if(paused&&pce_sgx_arena_sprites())
                overlay_call(0x78,pce_sgx_arena_hide_body);
#endif
            if(paused)pause_show();else pause_hide();
            simulation_tick=pce_ticks;
            continue;
        }
        if(paused){simulation_tick=pce_ticks;continue;}
#else
        if(pressed&KEY_RUN) {
            if(menu) {
                menu=0;
                if(selected!=stage||hero!=menu_hero||pce_campaign.diagnostic!=menu_mode) {
                    audio_stop();
                    if(!change_stage(selected))for(;;){};
                } else {
                    video_restore();
                    if(stage==2){overlay_call(0x6d,road_start);if(!pce_control.ok)for(;;){};video_display(true);}
                    if(pce_campaign.state==CAM_STORY)overlay_call(0x71,story_start);
                }
                previous=keys;simulation_tick=pce_ticks;
            }
            else {
                if(pce_campaign.state==CAM_STORY)
                    overlay_call(0x6e,story_graphics_restore);
                menu=1;selected=stage;menu_hero=hero;menu_mode=pce_campaign.diagnostic;
                if(stage==2){pce_raster_enabled=0;video_scene(&pce_scenes[1]);video_background(0);video_display(true);}
            }
        }
        if(menu) {
            if(pressed&KEY_2)pce_campaign.diagnostic^=1;
            if(pressed&KEY_LEFT)selected=selected==1?7:selected-1;
            if(pressed&KEY_RIGHT)selected=selected==7?1:selected+1;
            if(pressed&KEY_UP)hero=(hero+1)&3;
            if(pressed&KEY_DOWN)hero=(hero-1)&3;
            pce_metrics.hero=hero;menu_draw();continue;
        }
#endif
        pce_control.keys=keys;pce_control.pressed=pressed;pce_control.elapsed=elapsed;
        pce_control.power=power;
        if(!pce_campaign.diagnostic) {
            if(pce_campaign.state==CAM_STORY){overlay_call(0x71,story_step);simulation_tick=pce_ticks;continue;}
            if(pce_campaign.state==CAM_POWER){overlay_call(0x7b,power_frame);simulation_tick=pce_ticks;continue;}
            if(pce_campaign.state==CAM_OVER||pce_campaign.state==CAM_END) {
                if(pce_campaign.state==CAM_OVER) {
                    ui_fade_out();   /* a life lost for good: to black, then the continue screen */
                    pce_control.ok=0;
                    if(pce_continues)overlay_call(0x70,frontend_continue);
                    if(pce_control.ok) {
                        /* A continue restarts the current stage (or race phase) with fresh lives. */
                        pce_campaign.state=CAM_PLAY;pce_campaign.result=2;pce_metrics.hp=campaign_hearts();
                        simulation_tick=pce_ticks;continue;
                    }
                    if(pce_continues)ui_fade_out();   /* the countdown ran out on the continue panel */
                    overlay_call(0x71,frontend_game_over);
                } else overlay_call(0x71,frontend_credits);
                pce_control.stage=0;pce_control.hero=hero;
                overlay_call(0x72,frontend_start);if(!pce_control.ok)for(;;){};
                hero=pce_control.hero;
                if(!change_stage(1))for(;;){};
                simulation_tick=pce_ticks;
                continue;
            }
            if(pce_campaign.state==CAM_CLEAR) {
                if(stage!=6)overlay_call(0x72,frontend_victory);else audio_stop();   /* (the arena runs straight on into the cruiser: no victory painting, and the disc reads that follow want the music stopped) */
                if(stage==7){pce_campaign.state=CAM_END;continue;}
                pce_campaign.powers=2;
                if(!change_stage(stage+1))for(;;){};
                simulation_tick=pce_ticks;
                continue;
            }
            if(pce_campaign.result==2){
                if(audio_voice_active()){video_sat_begin();video_sat_end();continue;}
                uint8_t resume_phase_of_race=pce_metrics.phase,resume_wave=pce_campaign.wave;
                ui_fade_out();   /* the stage restarts from black (the platform stages fade in place: play_pce.c) */
                pce_campaign.result=0;resume_phase=stage==2?resume_phase_of_race:stage==6?resume_wave:0;
                if(!change_stage(stage))for(;;){};
                simulation_tick=pce_ticks;continue;}
            if(stage==2)overlay_call(0x79,race_frame);
            else if(stage==6)overlay_call(0x79,M6_FRAME);
            else if(stage==7)overlay_call(0x78,space_frame);
            else overlay_call(0x69,play_frame);
            if(pce_campaign.event){pce_campaign.event=0;overlay_call(0x71,story_start);}
            else if(pce_campaign.result==1){pce_campaign.state=CAM_CLEAR;pce_campaign.result=0;if(stage!=6)audio_music_once(6);}
            continue;
        }
#ifndef RETAIL
        if(stage==2)render_test(keys,pressed);
        else {
            pce_control.keys=keys;pce_control.pressed=pressed;pce_control.elapsed=elapsed;
            if(stage==6)overlay_call(0x79,M6_FRAME);else if(stage==7)overlay_call(0x78,space_frame);else overlay_call(0x69,play_frame);
        }
#endif
    }
}
