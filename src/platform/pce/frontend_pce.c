#include "frontend_pce.h"
#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "campaign_pce.h"
#include "audio_pcm.h"

/* Title, options and hero select. All run at 320x224 with baked art
 * (tools/pce/frontend.py). The tunnel backdrop animates by cycling four
 * palettes; hero panels change state by palette; menu items, names and the
 * chosen hero's portrait are hardware sprites, so they are not bound by the
 * per-character background palettes. Text is BG characters on a flat panel
 * colour. Shared services are in ui_pce.c; continue/credits are in credits_pce.c. */
#define UI_CODE __attribute__((noinline,section(".ram_bank114.text")))
extern uint8_t buffer[2048];
extern uint8_t previous;

static uint16_t ticks;
static const uint8_t name_pattern[4]=PCE_UI_NAME_PATTERNS,name_width[4]=PCE_UI_NAME_WIDTHS;
static const uint8_t arrow_left[5]=PCE_UI_ARROW_LEFT,arrow_right[5]=PCE_UI_ARROW_RIGHT;
static const uint8_t panel_x[4]={0x10,0x60,0xa0,0xe0};

/* ---------------------------------------------------------------- title */
UI_CODE static uint8_t title(bool resume) {
    ui_show(SCREEN_TITLE);pce_ui_state=1;
    audio_music(0);
    uint8_t items=resume?3:2,sel=0;
    uint8_t patch[2+44*8];
    uint16_t npatch;
    arcade_read(2,ui_screen->extra,patch,2);npatch=patch[0]|patch[1]<<8;
    if(npatch>44)npatch=44;
    arcade_read(2,ui_screen->extra+2,patch+2,npatch*8);
    static const uint16_t base[3]={PCE_UI_TITLE_START,PCE_UI_CONTINUE_PATTERN,PCE_UI_TITLE_OPTION};
    static const uint8_t width[3]={PCE_UI_TITLE_START_W,PCE_UI_CONTINUE_W,PCE_UI_TITLE_OPTION_W};
    ticks=0;
    for(;;) {
        video_wait();ui_read_keys();++ticks;
        if(ui_pressed&KEY_DOWN){sel=sel+1==items?0:sel+1;ui_blip();}
        if(ui_pressed&KEY_UP){sel=sel?sel-1:items-1;ui_blip();}
        if((ui_pressed&KEY_SELECT)&&resume){ui_end();return 3;}
        if(ui_pressed&(KEY_RUN|KEY_1|KEY_2)) {
            ui_blip();ui_end();
            if(!resume)return sel;
            return sel==1?3:sel==2?1:0;
        }
        video_sat_begin();
        for(uint16_t k=0;k<npatch;++k) {
            const uint16_t *p=(const uint16_t*)(patch+2)+k*4;
            ui_sprite(p[0],p[1],p[2],p[3],false);
        }
        /* Menu items: white, gold while selected (flashing white as in the Saturn). */
        for(uint8_t i=0;i<items;++i) {
            uint8_t kind=resume?i:(i?2:0);
            uint8_t on=(sel==i)&&((ticks&31)>=8);
            uint8_t w=width[kind];
            int16_t x=(320-w*16)/2,y=(resume?182:190)+i*12;
            for(uint8_t k=0;k<w;++k)ui_sprite(x+k*16,y,base[kind]+k,on?1:0,false);
        }
        video_sat_end();
    }
}

/* ---------------------------------------------------------------- hero select */
UI_CODE static void select_palettes(uint8_t chosen,bool glow) {
    for(uint8_t h=0;h<4;++h) {
        uint8_t state=h==chosen?(glow?2:1):0;
        arcade_read(2,ui_screen->extra+PCE_UI_RAMP_BYTES+(uint32_t)state*128+h*32,buffer,32);
        pce_vce_copy_palette(4+h,buffer,1);
    }
}
static uint8_t portrait_count;
static uint16_t portrait_pieces[50][4];
UI_CODE static void load_portrait(uint8_t hero) {
    const PceUiPortrait *p=&pce_ui_portrait[hero];
    portrait_count=p->count;
    ui_vram(p->patterns,UI_SPRITE_WORD+ui_screen->nsprpat*64,(uint32_t)p->count*128);
    arcade_read(2,p->pieces,portrait_pieces,(uint16_t)p->count*8);
    arcade_read(2,p->palette,buffer,512);
    pce_vce_copy_palette(16+3,buffer+3*32,13);
}
UI_CODE static uint8_t select_hero(uint8_t chosen) {
    ui_show(SCREEN_SELECT);pce_ui_state=2;
    audio_music(1);
    load_portrait(chosen);select_palettes(chosen,false);
    uint8_t confirmed=0;ticks=0;
    for(;;) {
        video_wait();ui_read_keys();++ticks;
        ui_cycle();
        if(!confirmed) {
            uint8_t before=chosen;
            if((ui_pressed&KEY_LEFT)&&chosen>0)--chosen;
            if((ui_pressed&KEY_RIGHT)&&chosen<3)++chosen;
            if(chosen!=before){ui_blip();load_portrait(chosen);}
            if(ui_pressed&(KEY_RUN|KEY_1|KEY_2)){confirmed=1;ticks=0;audio_pcm_play(2);}
        } else if(ticks>36) {
            /* Black fade-out, then the front end hands over to the NOW LOADING screen. */
            ui_fade(8);
            for(uint8_t level=1;level<8;++level){video_wait();video_wait();video_wait();ui_fade(level);}
            ui_end();return chosen;
        }
        select_palettes(chosen,(ticks&8)!=0||confirmed);
        video_sat_begin();
        for(uint8_t k=0;k<portrait_count;++k)
            ui_sprite(panel_x[chosen]+portrait_pieces[k][0],0x30+portrait_pieces[k][1],
                   ui_screen->nsprpat+portrait_pieces[k][2],portrait_pieces[k][3],false);
        for(uint8_t h=0;h<4;++h) {
            int16_t x=panel_x[h]+((h==0||h==3)?16:0);
            uint8_t pal=h==chosen?0:1,w=name_width[h],at=name_pattern[h];
            while(w>=2){ui_sprite(x,0xd0,at,pal,true);x+=32;at+=2;w-=2;}
            if(w)ui_sprite(x,0xd0,at,pal,false);
        }
        if(ticks&0x20) {
            ui_sprite(arrow_left[3],0x60+arrow_left[4],arrow_left[0],2,false);
            ui_sprite(arrow_left[3],0x60+arrow_left[4]+16,arrow_left[0]+1,2,false);
            ui_sprite(0x120+arrow_right[3],0x60+arrow_right[4],arrow_right[0],2,false);
            ui_sprite(0x120+arrow_right[3],0x60+arrow_right[4]+16,arrow_right[0]+1,2,false);
        }
        video_sat_end();
    }
}

/* ---------------------------------------------------------------- options */
enum { OPT_DIFFICULTY, OPT_LIVES, OPT_CONTINUES, OPT_MUSIC, OPT_EXIT, OPT_COUNT };
UI_CODE static void caps(uint8_t *lives,uint8_t *continues) {
    *lives=pce_options.difficulty==0?7:pce_options.difficulty==1?5:3;
    *continues=pce_options.difficulty==0?5:pce_options.difficulty==1?4:3;
}
UI_CODE static void option_row(uint8_t row,bool selected) {
    static const char *const difficulty[3]={"EASY  ","NORMAL","HARD  "};
    static const char *const music[4]={"OFF ","LOW ","MID ","HIGH"};
    static const char *const label[OPT_COUNT]={"DIFFICULTY","LIVES","CONTINUES","MUSIC VOLUME","EXIT"};
    uint8_t y=9+row*2;if(row==OPT_EXIT)y=20;
    ui_put(7,y,label[row],selected?13:12);
    if(row==OPT_EXIT)return;
    ui_put(24,y,selected?"<":" ",13);
    switch(row) {
    case OPT_DIFFICULTY:ui_put(26,y,difficulty[pce_options.difficulty],15);break;
    case OPT_LIVES:ui_put_number(26,y,pce_options.lives,15);ui_put(28,y,"    ",15);break;
    case OPT_CONTINUES:ui_put_number(26,y,pce_options.continues,15);ui_put(28,y,"    ",15);break;
    default:ui_put(26,y,music[pce_options.music&3],15);ui_put(30,y,"  ",15);break;
    }
    ui_put(33,y,selected?">":" ",13);
}
UI_CODE static void option_help(uint8_t row) {
    static const char *const help[OPT_COUNT]={
        "EASY 3  NORMAL 2  HARD 1 HEARTS","EXTRA LIVES AT THE START","CONTINUES AFTER GAME OVER",
        "CD MUSIC LEVEL","BACK TO THE TITLE"};
    ui_clear_rows(23,23);
    ui_put(4+(32-(uint8_t)__builtin_strlen(help[row]))/2,23,help[row],14);
}
UI_CODE static void options(void) {
    ui_show(SCREEN_OPTIONS);pce_ui_state=3;
    audio_music(3);           /* the options room has its own track, as in the main game */
    for(uint8_t r=0;r<OPT_COUNT;++r)option_row(r,r==0);
    option_help(0);
    uint8_t sel=0;ticks=0;
    for(;;) {
        video_wait();ui_read_keys();++ticks;
        ui_cycle();
        uint8_t old=sel,maxl=0,maxc=0;
        if(ui_pressed&KEY_UP){sel=sel?sel-1:OPT_COUNT-1;ui_blip();}
        if(ui_pressed&KEY_DOWN){sel=sel+1==OPT_COUNT?0:sel+1;ui_blip();}
        int8_t dir=(ui_pressed&KEY_RIGHT)?1:(ui_pressed&KEY_LEFT)?-1:0;
        if(dir) {
            bool changed=false;
            caps(&maxl,&maxc);
            switch(sel) {
            case OPT_DIFFICULTY:
                pce_options.difficulty=(pce_options.difficulty+3+dir)%3;
                caps(&maxl,&maxc);
                if(pce_options.lives>maxl)pce_options.lives=maxl;
                if(pce_options.continues>maxc)pce_options.continues=maxc;
                option_row(OPT_LIVES,false);option_row(OPT_CONTINUES,false);changed=true;break;
            case OPT_LIVES:
                if(dir>0&&pce_options.lives<maxl)++pce_options.lives;
                else if(dir<0&&pce_options.lives)--pce_options.lives;
                changed=true;break;
            case OPT_CONTINUES:
                if(dir>0&&pce_options.continues<maxc)++pce_options.continues;
                else if(dir<0&&pce_options.continues)--pce_options.continues;
                changed=true;break;
            case OPT_MUSIC:
                pce_options.music=(pce_options.music+4+dir)&3;
                if(pce_options.music)audio_music(3);else audio_stop();
                changed=true;break;
            default:break;
            }
            if(changed)ui_blip();
        }
        if(sel!=old){option_row(old,false);option_help(sel);}
        if(dir||sel!=old||ticks==1)option_row(sel,true);
        if(((ui_pressed&(KEY_RUN|KEY_1|KEY_2))&&sel==OPT_EXIT)||(ui_pressed&KEY_SELECT)) {
            ui_blip();pce_continues=pce_options.continues;ui_end();return;
        }
    }
}

/* ---------------------------------------------------------------- flow */
/* Title -> (options) -> hero select. pce_control.stage holds the saved
 * checkpoint on entry; on exit it holds the stage to start (0 = new game)
 * and pce_control.hero the choice. */
UI_CODE void frontend_start(void) {
    uint8_t checkpoint=pce_control.stage,chosen=pce_control.hero;
    previous=0;pce_metrics.ready=0;
    if(!loader_ui()){pce_control.ok=0;return;}
    for(;;) {
        uint8_t choice=title(checkpoint!=0);
        if(choice==1){options();continue;}
        if(choice==3){pce_control.stage=checkpoint;break;}
        chosen=select_hero(chosen);pce_control.stage=0;break;
    }
    audio_stop();pce_ui_state=0;pce_control.hero=chosen;pce_continues=pce_options.continues;
    pce_campaign.lives=pce_options.lives;pce_campaign.powers=2;pce_campaign.score=0;
    pce_control.ok=1;previous=ui_held;
}
