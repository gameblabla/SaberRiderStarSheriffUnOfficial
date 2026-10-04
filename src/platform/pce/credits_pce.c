#include "frontend_pce.h"
#include "ui_pce.h"
extern uint8_t buffer[2048];
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "campaign_pce.h"
#include "audio_pcm.h"

/* Continue, game over and credits share the options panel. */
#define CREDITS_CODE __attribute__((noinline,section(".ram_bank113.text")))
extern uint8_t previous;
extern volatile uint16_t pce_scroll_x;

CREDITS_CODE static void panel(void) {
    ui_show(SCREEN_PANEL);pce_ui_state=4;ui_clear_rows(7,24);
}
CREDITS_CODE void frontend_continue(void) {
    previous=0;audio_stop();
    if(!loader_ui()){pce_control.ok=0;return;}
    panel();
    /* The panel's text area is columns 4-35 and rows 7-24: its centre is column 20 (x=160), row 16. */
    char digit[2]={'0'+pce_continues%10,0};
    ui_put(15,10,"CONTINUE ?",13);
    ui_put(9,18,"PRESS START TO CONTINUE",12);
    ui_put(12,21,"CONTINUES LEFT",14);ui_put(27,21,digit,15);
    uint8_t seconds=15,clock=0;
    pce_control.ok=0;
    for(;;) {
        video_wait();ui_read_keys();ui_cycle();
        if(!clock)ui_put_number(19,14,seconds,13);
        if(++clock==60){clock=0;if(!seconds--)break;ui_blip();}
        if(ui_pressed&(KEY_RUN|KEY_1|KEY_2)){pce_control.ok=1;break;}
    }
    ui_end();audio_stop();pce_ui_state=0;
    if(pce_control.ok){--pce_continues;pce_campaign.lives=pce_options.lives;pce_campaign.powers=2;}
    previous=ui_held;
}
/* GAME OVER: the supplied Nemesis painting filling the screen, the lettering added as light by OR-ing bit planes
 * (frontend.py); its pulse rewrites the few palettes under the letters. Faded in and out. */
CREDITS_CODE void frontend_game_over(void) {
    previous=0;audio_stop();
    if(!loader_ui())return;
    ui_dark=1;ui_show(SCREEN_GAMEOVER);ui_dark=0;pce_ui_state=4;
    ui_fade(8);ui_fade(7);video_display(true);
    audio_music_once(4);           /* the jingle plays once, then silence */
    static const uint8_t pulse[8]={0,1,2,3,3,2,1,0};
    uint8_t level=7,shown=0;bool leaving=false;
    for(uint16_t t=0;;++t) {
        video_wait();ui_read_keys();
        if(!level&&!leaving&&!(t&7)&&pulse[(t>>3)&7]!=shown) {
            shown=pulse[(t>>3)&7];
            arcade_read(2,PCE_UI_GAMEOVER_GLOW+(uint32_t)shown*PCE_UI_GAMEOVER_SLOTS*32,buffer,PCE_UI_GAMEOVER_SLOTS*32);
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
CREDITS_CODE void frontend_credits(void) {
    previous=0;audio_stop();
    if(!loader_ui())return;
    panel();audio_music(9);
    uint16_t offsets[PCE_UI_CREDIT_PAGES];
    arcade_read(2,PCE_UI_CREDITS+2,offsets,PCE_UI_CREDIT_PAGES*2);
    for(uint8_t page=0;page<PCE_UI_CREDIT_PAGES;++page) {
        char text[160];
        arcade_read(2,PCE_UI_CREDITS+offsets[page],text,160);
        text[159]=0;
        uint8_t lines=1;
        for(const char *c=text;*c;++c)if(*c=='\n')++lines;
        ui_clear_rows(8,22);
        uint8_t row=12-lines/2;
        char *line=text;
        while(*line&&row<22) {
            char *end=line;while(*end&&*end!='\n')++end;
            char saved=*end;*end=0;
            ui_put(4+(32-(uint8_t)__builtin_strlen(line))/2,row++,line,page<2?13:12);
            if(!saved)break;
            line=end+1;
        }
        bool quit=false;
        for(uint16_t t=0;t<210&&!quit;++t) {
            video_wait();ui_read_keys();ui_cycle();
            if(t>20&&(ui_pressed&(KEY_RUN|KEY_1|KEY_2)))quit=true;
        }
        if(quit)break;
    }
    ui_end();audio_stop();pce_ui_state=0;previous=ui_held;
}
