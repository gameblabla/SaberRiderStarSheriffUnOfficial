#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "road_pce.h"
#include "play_pce.h"
#include "save_pce.h"
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
static uint8_t stage, hero, selected, menu, heading, phase;
static uint16_t race_x=4696,race_y=4096;
static uint8_t simulation_tick;
static uint8_t menu_hero,menu_mode;
void race_briefing_frame(void);
static const int8_t sine[32] PCE_TABLE={0,25,49,71,90,106,117,125,127,125,117,106,90,71,49,25,0,-25,-49,-71,-90,-106,-117,-125,-127,-125,-117,-106,-90,-71,-49,-25};
PCE_FLOW static bool change_stage(uint8_t n) {
    pce_control.stage=n;overlay_call(0x71,frontend_card);
    pce_metrics.ready=0;stage=n;phase=heading=0;race_x=4696;race_y=4096;
    pce_metrics.phase=0;pce_metrics.camera_x=pce_metrics.hp=0;
    if(!loader_scene(stage))return false;
    if(stage==6)m6_load();   /* Ramrod's arena's code images, into the banks the scene load has just put back */
    if(!loader_voice(hero))return false;
    pce_control.stage=stage;pce_control.hero=hero;pce_control.phase=0;
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
        if(stage==2||stage==6) {pce_campaign.story=0;overlay_call(0x71,story_start);}
    }
    audio_music(stage==1?5:stage==2?10:stage==3?13:stage==4?15:stage==5?14:stage==6?16:12);
    ui_black();ui_fade_in();   /* every stage comes up from black (the common transition) */
    pce_metrics.ready=1;return true;
}
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
            save_store(stage,hero,phase);
        }
        pce_control.x=race_x;pce_control.y=race_y;pce_control.heading=heading<<2;pce_control.phase=phase;
        overlay_call(0x6d,road_draw);
    }
    pce_metrics.phase=phase;pce_metrics.player_x=race_x;pce_metrics.player_y=race_y;
}
PCE_FLOW void flow_main(void) {
    pce_metrics.magic[0]='S';pce_metrics.magic[1]='R';pce_metrics.magic[2]='P';pce_metrics.magic[3]='C';
    pce_metrics.version=1;video_init();audio_pcm_init();
    if(!loader_font())for(;;){}
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
    PceSave checkpoint;
    bool resumed=save_load(&checkpoint);
    if(resumed)hero=checkpoint.hero;
    pce_control.stage=resumed?checkpoint.stage:0;pce_control.hero=hero;
    overlay_call(0x72,frontend_start);if(!pce_control.ok)for(;;){};
    if(!loader_font())for(;;){}
    hero=pce_control.hero;
    uint8_t initial=pce_control.stage?pce_control.stage:PCE_START_STAGE;
    if(!change_stage(initial))for(;;){}
    if(resumed&&initial==checkpoint.stage&&stage==2&&checkpoint.phase){
        phase=1;pce_metrics.phase=1;race_x=4096;heading=8;
        if(!pce_campaign.diagnostic){pce_control.phase=1;overlay_call(0x79,race_start);pce_campaign.state=CAM_PLAY;pce_campaign.timer=0;video_restore();}
    }
    simulation_tick=pce_ticks;
    for(;;) {
        /* The race runs as fast as it can (a pass is rarely a whole number of frames, and waiting out the rest wastes the
         * time the floor wants): the wait is only for the first tick after the last pass began. */
        if(stage==2){while(pce_ticks==simulation_tick){}++pce_metrics.frames;}else video_wait();
        uint8_t elapsed=pce_ticks-simulation_tick;simulation_tick=pce_ticks;
        if(elapsed>12)elapsed=12;
        if(pce_stall){pce_stall=0;elapsed=1;}   /* a CD seek or a big upload held the loop up: its ticks are not caught up */
        uint8_t keys=~pce_joypad_read(),pressed=keys&~previous;previous=keys;audio_tick();
        if(pressed&KEY_RUN) {
            if(menu) {
                menu=0;
                if(selected!=stage||hero!=menu_hero||pce_campaign.diagnostic!=menu_mode) {
                    audio_stop();
                    if(!loader_font()||!change_stage(selected))for(;;){};
                    save_store(stage,hero,0);
                } else {
                    video_restore();
                    if(stage==2){overlay_call(0x6d,road_start);if(!pce_control.ok)for(;;){};}
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
        pce_control.keys=keys;pce_control.pressed=pressed;pce_control.elapsed=elapsed;
        if(!pce_campaign.diagnostic) {
            if(pce_campaign.state==CAM_STORY){overlay_call(0x71,story_step);simulation_tick=pce_ticks;continue;}
            if(pce_campaign.state==CAM_POWER){overlay_call(0x69,play_present);continue;}
            if(pce_campaign.state==CAM_OVER||pce_campaign.state==CAM_END) {
                if(pce_campaign.state==CAM_OVER) {
                    ui_fade_out();   /* a life lost for good: to black, then the continue screen */
                    pce_control.ok=0;
                    if(pce_continues)overlay_call(0x71,frontend_continue);
                    if(pce_control.ok) {
                        /* A continue restarts the current stage (or race phase) with fresh lives. */
                        if(!loader_font())for(;;){}
                        pce_campaign.state=CAM_PLAY;pce_campaign.result=2;pce_metrics.hp=campaign_hearts();
                        simulation_tick=pce_ticks;continue;
                    }
                    if(pce_continues)ui_fade_out();   /* the countdown ran out on the continue panel */
                    overlay_call(0x71,frontend_game_over);
                } else overlay_call(0x71,frontend_credits);
                pce_control.stage=0;pce_control.hero=hero;
                overlay_call(0x72,frontend_start);if(!pce_control.ok)for(;;){};
                if(!loader_font())for(;;){}
                hero=pce_control.hero;
                if(!change_stage(1))for(;;){};
                save_store(1,hero,0);simulation_tick=pce_ticks;
                continue;
            }
            if(pce_campaign.state==CAM_CLEAR) {
                overlay_call(0x72,frontend_victory);
                if(stage==7){pce_campaign.state=CAM_END;continue;}
                pce_campaign.powers=2;
                if(!loader_font()||!change_stage(stage+1))for(;;){};
                save_store(stage,hero,0);simulation_tick=pce_ticks;
                continue;
            }
            if(pce_campaign.result==2){
                if(!(pce_cdb_adpcm_status()&ADPCM_STOPPED)){video_sat_begin();video_sat_end();continue;}
                uint8_t resume_phase=pce_metrics.phase,resume_wave=pce_campaign.wave;
                ui_fade_out();   /* the stage restarts from black (the platform stages fade in place: play_pce.c) */
                pce_campaign.result=0;if(!change_stage(stage))for(;;){};
                if(stage==2&&resume_phase){pce_control.phase=1;overlay_call(0x79,race_start);pce_campaign.state=CAM_PLAY;pce_campaign.timer=0;}
                if(stage==6){pce_control.phase=resume_wave;overlay_call(0x79,M6_START);pce_campaign.state=CAM_PLAY;}
                simulation_tick=pce_ticks;continue;}
            if(stage==2)overlay_call(0x79,race_frame);
            else if(stage==6)overlay_call(0x79,M6_FRAME);
            else if(stage==7)overlay_call(0x78,space_frame);
            else overlay_call(0x69,play_frame);
            if(pce_campaign.event){pce_campaign.event=0;overlay_call(0x71,story_start);}
            else if(pce_campaign.result==1){pce_campaign.state=CAM_CLEAR;pce_campaign.result=0;audio_music_once(6);}
            continue;
        }
        if(stage==2)render_test(keys,pressed);
        else {
            pce_control.keys=keys;pce_control.pressed=pressed;pce_control.elapsed=elapsed;
            if(stage==6)overlay_call(0x79,M6_FRAME);else if(stage==7)overlay_call(0x78,space_frame);else overlay_call(0x69,play_frame);
        }
    }
}
