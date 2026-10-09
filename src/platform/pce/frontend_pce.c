#include "frontend_pce.h"
#include "ui_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "campaign_pce.h"
#include "audio_pcm.h"
#include "sgx_pce.h"

/* Title, options and hero select. All run at 320x224 with baked art
 * (tools/pce/frontend.py). The tunnel backdrop animates by cycling four
 * palettes; hero panels change state by palette; menu items, names and the
 * chosen hero's portrait are hardware sprites, so they are not bound by the
 * per-character background palettes. Text is BG characters on a flat panel
 * colour. Shared services are in ui_pce.c; continue/credits are in credits_pce.c. */
#define UI_CODE __attribute__((noinline,section(".ram_bank114.text"),minsize))
extern uint8_t buffer[2048];
extern uint8_t previous;

static uint16_t ticks;
static const uint8_t name_pattern[4]=PCE_UI_NAME_PATTERNS,name_width[4]=PCE_UI_NAME_WIDTHS;
static const uint8_t arrow_left[5]=PCE_UI_ARROW_LEFT,arrow_right[5]=PCE_UI_ARROW_RIGHT;
static const uint8_t panel_x[4]={0x10,0x60,0xa0,0xe0};

/* ---------------------------------------------------------------- title */
UI_CODE static void title_draw(const uint8_t *patch,uint16_t patch_count,uint8_t sel) {
    static const uint16_t base[2]={PCE_UI_TITLE_START,PCE_UI_TITLE_OPTION};
    static const uint8_t width[2]={PCE_UI_TITLE_START_W,PCE_UI_TITLE_OPTION_W};
    uint8_t items=2;
    video_sat_begin();
    for(uint16_t k=0;k<patch_count;++k) {
        const uint16_t *p=(const uint16_t*)(patch+2)+k*4;
        ui_sprite(p[0],p[1],p[2],p[3],false);
    }
    /* Menu items: white, gold while selected (flashing white as in the Saturn). */
    for(uint8_t i=0;i<items;++i) {
        uint8_t kind=i;
        uint8_t on=(sel==i)&&((ticks&31)>=8);
        uint8_t w=width[kind];
        int16_t x=(320-w*16)/2,y=190+i*12;
        for(uint8_t k=0;k<w;++k)ui_sprite(x+k*16,y,base[kind]+k,on?1:0,false);
    }
    video_sat_end();
}
UI_CODE static uint8_t title(void) {
    ui_dark=1;ui_show(SCREEN_TITLE);ui_dark=0;pce_ui_state=1;
    audio_music(0);
    uint8_t items=2,sel=0;
    uint8_t patch[2+44*8];
    uint16_t patch_count;
    arcade_read(2,ui_screen->extra,patch,2);patch_count=patch[0]|patch[1]<<8;
    if(patch_count>44)patch_count=44;
    arcade_read(2,ui_screen->extra+2,patch+2,patch_count*8);
    ticks=0;
    title_draw(patch,patch_count,sel);ui_black();ui_fade_in();   /* up from black, as every screen */
    for(;;) {
        video_wait();ui_read_keys();++ticks;
        if(ui_pressed&KEY_DOWN){sel=sel+1==items?0:sel+1;ui_blip();}
        if(ui_pressed&KEY_UP){sel=sel?sel-1:items-1;ui_blip();}
        if(ui_pressed&(KEY_RUN|KEY_1|KEY_2)) {
            ui_blip();ui_fade_out();ui_end();   /* fade to black, then the next screen comes up from black */
            return sel;
        }
        title_draw(patch,patch_count,sel);
    }
}

/* ---------------------------------------------------------------- hero select */
UI_CODE static void select_palettes(uint8_t chosen,bool glow) {
#ifdef PCE_SGX
    if(pce_sgx_active)return; /* SGX foreground owns immutable BG palettes 6..15. */
#endif
    for(uint8_t h=0;h<4;++h) {
        uint8_t state=h==chosen?(glow?2:1):0;
        arcade_read(2,ui_screen->extra+PCE_UI_RAMP_BYTES+(uint32_t)state*128+h*32,buffer,32);
        pce_vce_copy_palette(4+h,buffer,1);
    }
}
/* A portrait's patterns take over a frame to copy, so two buffers alternate: the next hero is copied to the idle
 * one while the old portrait keeps showing, then pieces (SAT) and palettes switch together right after VBlank.
 * Slot 0 follows the screen's own sprite patterns, slot 1 sits just below the sprite area (free on this screen). */
#define PORTRAIT_MAX 50
static uint8_t portrait_count,portrait_slot;
static uint16_t portrait_pieces[PORTRAIT_MAX][4];
UI_CODE static void portrait_patterns(uint8_t hero,uint8_t slot) {
#ifdef PCE_SGX
    if(pce_sgx_active) {
        pce_sgx_select_hero=hero;overlay_call(0x6e,pce_sgx_select_body);return;
    }
#endif
    const PceUiPortrait *p=&pce_ui_portrait[hero];
    ui_vram_load(p->patterns,
        slot?UI_SPRITE_WORD-PORTRAIT_MAX*64:UI_SPRITE_WORD+ui_screen->nsprpat*64,
        (uint32_t)p->count*128);
}
UI_CODE static void portrait_pieces_use(uint8_t hero,uint8_t slot) {
#ifdef PCE_SGX
    if(pce_sgx_active){portrait_count=0;return;}
#endif
    const PceUiPortrait *p=&pce_ui_portrait[hero];
    portrait_count=p->count;portrait_slot=slot;
    arcade_read(2,p->pieces,portrait_pieces,(uint16_t)p->count*8);
}
UI_CODE static void portrait_palette(uint8_t hero) {
#ifdef PCE_SGX
    if(pce_sgx_active)return;
#endif
    arcade_read(2,pce_ui_portrait[hero].palette,buffer,512);
    pce_vce_copy_palette(16+3,buffer+3*32,13);
}
UI_CODE static void select_draw(uint8_t shown,bool due,uint8_t chosen,uint16_t ticks_now) {
    video_sat_begin();
    for(uint8_t k=0;k<portrait_count;++k)
        ui_sprite(panel_x[due?chosen:shown]+portrait_pieces[k][0],0x30+portrait_pieces[k][1],
               portrait_slot?portrait_pieces[k][2]-PORTRAIT_MAX:ui_screen->nsprpat+portrait_pieces[k][2],portrait_pieces[k][3],false);
    for(uint8_t h=0;h<4;++h) {
        int16_t x=panel_x[h]+((h==0||h==3)?16:0);
        uint8_t pal=h==(due?chosen:shown)?0:1,w=name_width[h],at=name_pattern[h];
        while(w>=2){ui_sprite(x,0xd0,at,pal,true);x+=32;at+=2;w-=2;}
        if(w)ui_sprite(x,0xd0,at,pal,false);
    }
    if(ticks_now&0x20) {
        ui_sprite(arrow_left[3],0x60+arrow_left[4],arrow_left[0],2,false);
        ui_sprite(arrow_left[3],0x60+arrow_left[4]+16,arrow_left[0]+1,2,false);
        ui_sprite(0x120+arrow_right[3],0x60+arrow_right[4],arrow_right[0],2,false);
        ui_sprite(0x120+arrow_right[3],0x60+arrow_right[4]+16,arrow_right[0]+1,2,false);
    }
    video_sat_end();
}
UI_CODE static uint8_t select_hero(uint8_t chosen) {
#ifdef PCE_SGX
    if(!loader_ui_animation()){pce_control.ok=0;return chosen;}
#endif
    ui_dark=1;ui_show(SCREEN_SELECT);ui_dark=0;pce_ui_state=2;
    portrait_patterns(chosen,0);portrait_pieces_use(chosen,0);portrait_palette(chosen);select_palettes(chosen,false);
    audio_music(1);
    uint8_t confirmed=0,shown=chosen;bool palettes_due=false;ticks=0;
    select_draw(shown,false,chosen,0);ui_black();ui_fade_in();   /* up from black */
    for(;;) {
        video_wait();ui_read_keys();++ticks;
        /* The SAT with the new portrait went out at this VBlank: its palettes follow immediately. */
        if(palettes_due){portrait_palette(chosen);shown=chosen;palettes_due=false;}
        select_palettes(shown,(ticks&8)!=0||confirmed);
        ui_cycle();
        if(!confirmed) {
            if(!palettes_due&&shown==chosen) {
                if((ui_pressed&KEY_LEFT)&&chosen>0)--chosen;
                if((ui_pressed&KEY_RIGHT)&&chosen<3)++chosen;
                if(chosen!=shown) {
                    ui_blip();
                    portrait_patterns(chosen,portrait_slot^1);
                    portrait_pieces_use(chosen,portrait_slot^1);palettes_due=true;
                }
            }
            if(ui_pressed&(KEY_RUN|KEY_1|KEY_2)){confirmed=1;ticks=0;audio_pcm_play(2);}
        } else if(ticks>36) {
            /* Black fade-out, then the front end hands over to the NOW LOADING screen. */
            ui_fade_out();
            ui_end();return chosen;
        }
        select_draw(shown,palettes_due,chosen,ticks);
    }
}

/* ---------------------------------------------------------------- options */
#if DEBUG
static uint8_t start_level=1;
#endif
enum { OPT_DIFFICULTY, OPT_LIVES, OPT_CONTINUES, OPT_MUSIC, OPT_SWAP,
#if DEBUG
    OPT_LEVEL,
#endif
    OPT_EXIT, OPT_COUNT };
UI_CODE static void caps(uint8_t *lives,uint8_t *continues) {
    *lives=pce_options.difficulty==0?7:pce_options.difficulty==1?5:3;
    *continues=pce_options.difficulty==0?5:pce_options.difficulty==1?4:3;
}
UI_CODE static void option_row(uint8_t row,bool selected) {
    static const char *const difficulty[3]={"EASY  ","NORMAL","HARD  "};
    static const char *const music[2]={"OFF ","ON  "};
#if DEBUG
    static char level_text[3]="01";
#endif
    static const char *const label[OPT_COUNT]={"DIFFICULTY","LIVES","CONTINUES","MUSIC","SWAP I/II",
#if DEBUG
        "LEVEL",
#endif
        "EXIT"};
    uint8_t y=9+row*2;
#if DEBUG
    y=8+row*2;
#endif
    if(row==OPT_EXIT)y=20;
    ui_put(7,y,label[row],selected?13:12);
    if(row==OPT_EXIT)return;
    ui_put(24,y,selected?"<":" ",13);
    switch(row) {
    case OPT_DIFFICULTY:ui_put(26,y,difficulty[pce_options.difficulty],15);break;
    case OPT_LIVES:ui_put_number(26,y,pce_options.lives,15);ui_put(28,y,"    ",15);break;
    case OPT_CONTINUES:ui_put_number(26,y,pce_options.continues,15);ui_put(28,y,"    ",15);break;
    case OPT_MUSIC:ui_put(26,y,music[pce_options.music!=0],15);ui_put(30,y,"  ",15);break;
#if DEBUG
    case OPT_LEVEL:level_text[1]='0'+start_level;ui_put(26,y,level_text,15);ui_put(28,y,"    ",15);break;
#endif
    case OPT_SWAP:ui_put(26,y,music[pce_options.swap_buttons!=0],15);ui_put(30,y,"  ",15);break;
    default:break;
    }
    ui_put(33,y,selected?">":" ",13);
}
UI_CODE static void option_help(uint8_t row) {
    static const char *const help[OPT_COUNT]={
        "EASY 3  NORMAL 2  HARD 1 HEARTS","EXTRA LIVES AT THE START","CONTINUES AFTER GAME OVER",
        "CD MUSIC ON OR OFF","SWAP BUTTONS I AND II",
#if DEBUG
        "START A NEW GAME AT THIS LEVEL",
#endif
        "BACK TO THE TITLE"};
    ui_clear_rows(23,23);
    ui_put(4+(32-(uint8_t)__builtin_strlen(help[row]))/2,23,help[row],14);
}
UI_CODE static void options(void) {
    ui_dark=1;ui_show(SCREEN_OPTIONS);ui_dark=0;pce_ui_state=3;
    audio_music(3);           /* the options room has its own track, as in the main game */
    for(uint8_t r=0;r<OPT_COUNT;++r)option_row(r,r==0);
    option_help(0);
    uint8_t sel=0;ticks=0;
    ui_black();ui_fade_in();
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
                pce_options.music=pce_options.music?0:3;
                if(pce_options.music)audio_music(3);else audio_stop();
                changed=true;break;
#if DEBUG
            case OPT_LEVEL:
                start_level=dir>0?(start_level==7?1:start_level+1):(start_level==1?7:start_level-1);
                changed=true;break;
#endif
            case OPT_SWAP:
                pce_options.swap_buttons^=1;
                changed=true;break;
            default:break;
            }
            if(changed)ui_blip();
        }
        if(sel!=old){option_row(old,false);option_help(sel);}
        if(dir||sel!=old||ticks==1)option_row(sel,true);
        if(((ui_pressed&(KEY_RUN|KEY_1|KEY_2))&&sel==OPT_EXIT)||(ui_pressed&KEY_SELECT)) {
            ui_blip();pce_continues=pce_options.continues;ui_fade_out();ui_end();return;
        }
    }
}

/* ---------------------------------------------------------------- flow */
/* Title -> (options) -> hero select. Every selection starts a new game. */
UI_CODE void frontend_start(void) {
    uint8_t chosen=pce_control.hero;
    /* A button held through GAME OVER's fade must not start the title or
     * confirm a hero again. Wait for a fresh press on the new screen. */
    previous=~pce_joypad_read();pce_metrics.ready=0;
    if(!loader_ui()){pce_control.ok=0;return;}
    for(;;) {
        uint8_t choice=title();
        if(choice==1){options();continue;}
        chosen=select_hero(chosen);
#if DEBUG
        pce_control.stage=start_level;
#else
        pce_control.stage=0;
#endif
        break;
    }
    audio_stop();pce_ui_state=0;pce_control.hero=chosen;pce_continues=pce_options.continues;
    pce_campaign.lives=pce_options.lives;pce_campaign.powers=2;pce_campaign.score=0;
    pce_control.ok=1;previous=ui_held;
}
