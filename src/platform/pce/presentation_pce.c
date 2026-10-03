#include "presentation_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#define PRESENT __attribute__((noinline,section(".ram_bank116.text")))
volatile uint8_t pce_ui_state;
extern uint8_t previous;
static const char *const names[4]={"SABER RIDER","FIREBALL","APRIL","COLT"};
static char brief[1024];
PRESENT static void ui_draw(uint8_t page) {
    video_display(false);video_scene(&pce_ui_scenes[page]);
    video_background(0);video_sat_begin();video_sat_end();video_display(true);
}
PRESENT void frontend_start(void) {
    uint8_t checkpoint=pce_control.stage,chosen=pce_control.hero;
    previous=0;pce_metrics.ready=0;
    if(!loader_ui()){pce_control.ok=0;return;}
    pce_ui_state=1;ui_draw(0);audio_music(0);
    video_text(6,22,"RUN / I START GAME");
    if(checkpoint)video_text(5,24,"SELECT CONTINUE SAVE");
    char *line=brief;uint8_t briefing_done=0;
    for(;;) {
        video_wait();uint8_t keys=~pce_joypad_read(),pressed=keys&~previous;previous=keys;
        if(pce_ui_state==1) {
            if((pressed&KEY_SELECT)&&checkpoint){pce_control.stage=checkpoint;break;}
            if(pressed&(KEY_RUN|KEY_1|KEY_2)) {
                pce_ui_state=2;ui_draw(1+chosen);audio_music(1);
                video_text(5,2,"SELECT YOUR SHERIFF");video_text(9,24,names[chosen]);
                video_text(4,26,"LEFT/RIGHT  I/II ACCEPT");
            }
        } else if(pce_ui_state==2) {
            if(pressed&(KEY_LEFT|KEY_RIGHT)) {
                chosen=(chosen+(pressed&KEY_RIGHT?1:3))&3;ui_draw(1+chosen);
                video_text(5,2,"SELECT YOUR SHERIFF");video_text(9,24,names[chosen]);
                video_text(4,26,"LEFT/RIGHT  I/II ACCEPT");
            }
            if(pressed&(KEY_RUN|KEY_1|KEY_2)) {
                pce_ui_state=3;ui_draw(5);audio_music(2);
                arcade_read(2,PCE_BRIEFING_TEXT,brief,sizeof brief);brief[1023]=0;
                line=brief;briefing_done=0;
            }
        }
        if(pce_ui_state==3&&(!briefing_done||(pressed&(KEY_RUN|KEY_1|KEY_2)))) {
            if(briefing_done&&!*line){pce_control.stage=1;break;}
            video_panel(1,19,30,8);
            for(uint8_t row=0;row<6&&*line;++row) {
                char *end=line;while(*end&&*end!='\n')++end;
                bool more=*end;*end=0;video_text(2,20+row,line);line=end+more;
            }
            video_text(22,26,"I/II NEXT");briefing_done=1;
        }
    }
    audio_stop();pce_ui_state=0;pce_control.hero=chosen;pce_control.ok=1;
}
PRESENT void presentation_frame(void) {
    const uint16_t *base=pce_present_base[pce_metrics.stage-1];
    if(base[0]) {
        uint8_t hp=pce_metrics.hp>3?3:pce_metrics.hp;
        video_sprite(base[1]+pce_campaign.lives%10,29,14,false,16);
        video_sprite(base[1]+pce_campaign.powers%10,60,14,false,16);
        video_sprite(base[0]+pce_control.hero*4+hp,0,0,false,16);
    }
}
void presentation_draw(void) {overlay_call(0x74,presentation_frame);video_front_mark();}

static uint8_t panel_x,panel_y,panel_w,panel_h;
PRESENT static void panel_draw(void) {
    uint8_t x=panel_x,y=panel_y,w=panel_w,h=panel_h;
    extern volatile uint16_t pce_scroll_x;
    for(uint8_t row=0;row<h;++row)for(uint8_t col=0;col<w;++col) {
        uint8_t tile=(row==0||row==h-1)?92:col==0?93:col==w-1?94:91;
        uint16_t address=pce_raster_enabled?(uint16_t)(48+y+row)*128+x+col:
            (uint16_t)(y+row)*64+(((pce_scroll_x>>3)+x+col)&63);
        video_vdc(0,address);video_vdc(2,0xf000+(PCE_FONT_WORD>>4)+tile);
    }
}

void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h) {
    panel_x=x;panel_y=y;panel_w=w;panel_h=h;overlay_call(0x74,panel_draw);
}
