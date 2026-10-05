#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "presentation_pce.h"
#include "play_pce.h"
#include "play_internal.h"
#include "sprite_cache_pce.h"
#include "scenery_pce.h"
#include "audio_pcm.h"
extern volatile uint16_t pce_sky_far,pce_sky_near;
#include <string.h>
#define STORY_CODE __attribute__((noinline,minsize,section(".ram_bank113.text")))
static uint32_t story_address;
static uint8_t page_count,story_y;   /* story_y: BG row of the box's top */
static char story_text[256];
static uint8_t race_colour;
/* Put wide box and glyphs in the race's unused VRAM. No sprite-cache pages
 * are needed for the box, and its 56-character width matches the dot clock. */
/* The box covers sky cells (BAT rows 53-58, columns 6-61) and its glyph characters sit on the BAT rows 24-47 that hold the road's wrap copies. The cells
 * the box takes are kept in Arcade RAM (the tile directory's area, unused in the race) and written back when the dialogue closes; the wrap copies come
 * from the road asset again. (Reloading the whole sky instead wiped the sky back in over 20 frames.) While the box is up no scanline may use a
 * wrap copy: the frozen road's classes are cut down to the plain two. */
#define RACE_KEPT 0x1e0000UL
static uint8_t race_kept;
extern uint16_t columns[33][30];
PCE_X2 static void race_keep(void) {
    uint16_t w[8];
    for(uint8_t row=0;row<6;++row)for(uint8_t part=0;part<7;++part) {
        uint16_t address=(uint16_t)(53+row)*128+6+part*8;
        pce_cpu_irq_disable();
        pce_vdc_index=1;*(volatile uint8_t*)0x20f7=1;*IO_VDC_INDEX=1;*IO_VDC_DATA_LO=address;*IO_VDC_DATA_HI=address>>8;
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;*IO_VDC_INDEX=2;
        for(uint8_t k=0;k<8;++k){uint8_t lo=*IO_VDC_DATA_LO;w[k]=lo|(uint16_t)*IO_VDC_DATA_HI<<8;}
        pce_cpu_irq_enable();
        arcade_write(2,RACE_KEPT+(uint32_t)row*112+part*16,w,16);
    }
    uint8_t *classes=(uint8_t*)columns+512;
    for(uint16_t i=0;i<256;++i)classes[i]&=1;
    race_kept=1;
}
PCE_X2 static void race_unbox(void) {
    if(!race_kept)return;
    for(uint8_t row=0;row<6;++row){pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;arcade_vram(RACE_KEPT+(uint32_t)row*112,(uint16_t)(53+row)*128+6,112);}
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(PCE_RACE_ROAD_BAT+6656,24*128,6144);   /* the wrap copies */
    race_kept=0;
}
PCE_X2 static void race_box(void) {   /* bank $77 (the CD buffer's second: $6f and $7c are full) */
    uint32_t record[4];uint16_t bytes;
    extern uint8_t buffer[2048];
    if(!race_kept)race_keep();
    uint32_t a=PCE_RACE_DIALOG_WIDE+(uint16_t)race_colour*18;
    arcade_read(2,a,record,16);arcade_read(2,a+16,&bytes,2);
    arcade_read(2,record[0],buffer,32);pce_vce_copy_palette(14,buffer,1);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(record[1],0x4000,bytes);
    arcade_vram(record[3],0x0c00,6144);
    arcade_read(2,record[2],buffer,672);
    for(uint8_t row=0;row<6;++row) {
        video_vdc(0,(uint16_t)(53+row)*128+6);
        for(uint8_t x=0;x<56;++x)video_vdc(2,((uint16_t*)buffer)[(uint16_t)row*56+x]);
    }
}
/* Platform panels use BG characters, leaving only four alpha corners in
 * the SAT. Their font and palette are restored from the preloaded archive. */
static uint16_t story_column;
static uint8_t platform_colour;
PCE_MISSION static void platform_box(void) {
    uint8_t colour=platform_colour;
    uint32_t record[4];uint16_t bytes;
    extern uint8_t buffer[2048];
    extern volatile uint16_t pce_scroll_x,pce_scroll_y;
    uint32_t a=pce_dialog_bg[pce_metrics.stage-1]+(uint16_t)(colour&3)*18;
    arcade_read(2,a,record,16);arcade_read(2,a+16,&bytes,2);
    arcade_read(2,record[0],buffer,32);pce_vce_copy_palette(15,buffer,1);   /* (queued: the VBlank that brings the sprite corners writes it) */
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(record[1],0x4000,bytes);
    arcade_vram(record[3],PCE_FONT_WORD,3072);
    arcade_read(2,record[2],buffer+1024,336);   /* the cells go in right after that VBlank (video_cells_apply) */
    story_column=pce_scroll_x>>3;
}
PCE_FLOW void story_graphics_restore(void) {   /* bank $6e: $6f is full */
    if(pce_metrics.stage==2||pce_metrics.stage==7)return;
    extern const PceScene *video_scene_ptr;
    uint8_t colors[32];
    arcade_read(2,video_scene_ptr->pal+15*32,colors,32);pce_vce_copy_palette(15,colors,1);
    if(pce_metrics.stage==6) {   /* the dialogue's corner pieces wrote their colours into palette 31, the arena HUD's: its own are loaded again */
        uint32_t entry;arcade_read(2,video_scene_ptr->sprites+(uint32_t)pce_hud_base[5]*16+8,&entry,4);
        arcade_read(2,entry,colors,32);pce_vce_copy_palette(31,colors,1);
    }
    pce_vce_set_color(255,0x1ff);
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(pce_dialog_original_font[pce_metrics.stage-1],PCE_FONT_WORD,3072);
}
/* Typewriter: the lines are kept NUL-terminated in story_text and revealed a character at a time. */
static char *line_text[4];static uint8_t line_row[4],line_count,type_line,type_col,type_clock;
STORY_CODE static uint32_t pointer(uint32_t a) {
    uint32_t p;arcade_read(2,a,&p,4);return p;
}
extern vdc_sprite_t sat[2][64];
extern uint8_t cut_phase;
void race_dialog_cars(void),space_dialog_ship(void);
extern uint8_t sat_page,sat_count;
extern volatile uint16_t pce_scroll_x,pce_scroll_y;
/* Platform panels and opaque-backed glyphs are BG tiles. Their four rounded
 * sprite corners retain the scenery behind them; other modes keep their own
 * panel renderer. */
STORY_CODE static void draw(void) {
    uint32_t a=pointer(story_address+1+(uint16_t)pce_campaign.page*4);
    uint16_t avatar;uint8_t colour;
    arcade_read(2,a,&avatar,2);arcade_read(2,a+2,&colour,1);
    arcade_read(2,a+3,story_text,sizeof story_text);story_text[255]=0;
    /* The box sits at the top on the platform stages (the hero stands where a bottom box would be) and on the race, at the bottom on the cruiser's. */
    uint8_t y=story_y=pce_metrics.stage==2||(pce_metrics.stage!=7&&!cut_phase)?5:20;   /* the race's box is BG rows 53-58 (video_text 48+y), at the top; the outrider's scene: the box goes below the actor the camera pans to */
    /* The cruiser's playfield is scrolled (a multiple of 8 dots down while it greets: space_pce.c): screen row 20 is that many rows on in the BAT. */
    if(pce_metrics.stage==7)y=story_y=(20+(pce_scroll_y>>3))&31;
    /* BG cells sit (scroll & 7) pixels left of their grid on a scrolling playfield. */
    int16_t box_x=24-(pce_raster_enabled?0:(pce_scroll_x&7)),box_y=(pce_metrics.stage==7?20:y)*8-(pce_metrics.stage!=2&&pce_metrics.stage<6?16:0);   /* platform sprites are baked 16 lines low */
    /* Blank every BG cell under the box except the four 2x2 corner blocks; the corner pieces stay in front of the
     * scenery so their rounded edges show the scenery, not a hole. */
    bool platform=pce_metrics.stage!=2&&pce_metrics.stage!=7;   /* the platform stages and Ramrod's arena: a scrolling background with the panel in BG characters and four sprite corners */
    if(pce_metrics.stage==2){pce_sky_far=pce_sky_near=0;race_colour=colour&3;overlay_call(0x77,race_box);}
    else if(!platform){video_panel(5,y,24,2);video_panel(3,y+2,28,2);video_panel(5,y+4,24,2);}
    video_sat_begin();
    if(platform)foreground_prepare();   /* the outpost's walls stay in front of the hero and the actors the scene shows */
    if(avatar!=65535) {
        video_sprite(avatar,pce_metrics.stage==2?(box_x-26)*2:box_x-26,box_y-8,false,16);
        /* Cache slots from 15 up share one hardware palette, which another portrait's upload overwrote since this one was
         * cached: load the speaker's own colours again. */
        extern const PceScene *video_scene_ptr;extern uint8_t sprite_slot_of[480];
        uint8_t slot=sprite_slot_of[avatar];
        if(slot<48) {
            uint32_t entry;uint8_t colors[32];
            arcade_read(2,video_scene_ptr->sprites+(uint32_t)avatar*16+8,&entry,4);
            arcade_read(2,entry,colors,32);pce_vce_copy_palette(16+(slot<15?slot:15),colors,1);
        }
    }
    uint8_t first=sat_count;
    uint16_t box=pce_dialog_base[pce_metrics.stage-1]+(colour&3)*2;
    if(platform) {
        dialog_corner_colour=colour&3;dialog_corner_y=y;
        overlay_call(0x78,dialog_corners);
    } else if(pce_metrics.stage!=2){video_sprite(box,box_x,box_y,false,16);video_sprite(box+1,box_x+112,box_y,false,16);}
    uint16_t left=0xffff,top=0xffff;
    for(uint8_t k=first;k<sat_count;++k){if(sat[sat_page][k].x<left)left=sat[sat_page][k].x;if(sat[sat_page][k].y<top)top=sat[sat_page][k].y;}
    for(uint8_t k=first;k<sat_count;++k) {
        vdc_sprite_t *e=&sat[sat_page][k];
        bool corner=(e->x==left||e->x==left+208)&&(e->y==top||e->y==top+32);
        if(!platform&&!corner)e->attr&=~VDC_SPRITE_FG;
    }
    /* The world stands still behind the text: the hero (behind the box, which comes first in the SAT), the actors
     * (the cutscene outrider stays put) and the boss stay on screen. */
    if(pce_metrics.stage==2)overlay_call(0x77,race_dialog_cars);   /* the car and the rivals stay on view behind the text */
    if(pce_metrics.stage==7)overlay_call(0x78,space_dialog_ship);
    if(platform&&pce_metrics.stage<6) {
        video_sprite(hero_sprite,player.x-camera,player.y-16,facing,16);
        overlay_call(0x74,actors_draw);
        if(!pce_campaign.diagnostic)overlay_call(0x70,combat_draw);
        foreground_draw();
    }
    /* The panel's graphics, palette and cells are prepared before the SAT is queued; the cells are written right after the VBlank that brings the sprite corners. */
    if(platform){platform_colour=colour;overlay_call(0x6f,platform_box);}
    video_sat_end();
    if(platform){video_wait();video_cells_apply(y,story_column);}
    char *line=story_text;uint8_t lines=0;
    for(char *c=line;;++c){if(*c=='\n'||!*c){++lines;if(!*c)break;}}
    /* One line sits mid-box; two lines get a blank row between them. */
    uint8_t row=y+(lines==1?2:1),pitch=lines==2?2:1;
    line_count=0;type_line=type_col=type_clock=0;
    for(;lines&&*line&&line_count<4;--lines,row+=pitch) {
        char *end=line;while(*end&&*end!='\n')++end;
        bool more=*end!=0;*end=0;
        line_text[line_count]=line;line_row[line_count++]=row;
        line=end+more;
    }
}
/* One character every other frame (a held button types four times faster, as the main game's 3x). */
STORY_CODE static bool typing(void) {
    if(type_line>=line_count)return false;
    type_clock+=pce_control.elapsed*((pce_control.keys&(KEY_1|KEY_2))?4:1);
    while(type_clock>=2&&type_line<line_count) {
        type_clock-=2;
        char c[2]={line_text[type_line][type_col++],0};
        if(!c[0]){++type_line;type_col=0;continue;}
        video_text(5+type_col-1,line_row[type_line],c);
        if(!line_text[type_line][type_col]){++type_line;type_col=0;}
    }
    return type_line<line_count;
}
STORY_CODE void story_start(void) {
    uint32_t dir=pce_scenes[pce_metrics.stage-1].story;
    uint8_t count;arcade_read(2,dir,&count,1);
    if(pce_campaign.story>=count){pce_campaign.state=CAM_PLAY;return;}
    story_address=pointer(dir+1+((uint16_t)pce_control.hero*count+pce_campaign.story)*4);
    arcade_read(2,story_address,&page_count,1);
    pce_campaign.page=0;pce_campaign.state=CAM_STORY;pce_campaign.timer=0;
    /* The race and the cockpits keep their HUD in sprites: let the last two displayed generations go (their cache slots
     * stay pinned through the SAT DMA) so the box and the avatar find slots in the same frame. */
    if(pce_metrics.stage==2||pce_metrics.stage==7)for(uint8_t k=0;k<3;++k){video_sat_begin();video_sat_end();video_wait();}
    /* Resuming from Run invalidates the BAT. Remove the menu's cells before
     * reopening a panel, including the cells outside its restoration area. */
    if(pce_metrics.stage!=2&&pce_metrics.stage<6)video_background(camera);
    /* Reset world rumble before submitting the unshifted box and actors. */
    if(pce_metrics.stage!=2&&pce_metrics.stage!=7)pce_scroll_y=0;   /* (the arena's last shake would leave the panel's cells off from its sprite corners) */
    draw();
}
STORY_CODE void story_step(void) {
    pce_campaign.timer+=pce_control.elapsed;
    if(typing())return;
    if(pce_campaign.timer<12||!(pce_control.pressed&(KEY_1|KEY_2)))return;
    pce_campaign.timer=0;
    audio_pcm_tick();   /* the PC dialogue's page close: sfx 0 */
    if(++pce_campaign.page<page_count)draw();
    else {
        /* Retained scenery must re-admit its chunks. Platform panels now need
         * only four corner patterns: keep the displayed generation pinned until
         * the closing SAT DMA, so uploads cannot overwrite its live graphics. */
        if(pce_metrics.stage==2||pce_metrics.stage==7) {
            memset(sprite_used,0,sizeof sprite_used);memset(sprite_pinned,0,sizeof sprite_pinned);
        }
        foreground_reset();
        pce_campaign.state=CAM_PLAY;
        if(pce_metrics.stage!=2&&pce_metrics.stage<6) {
            /* Put the blanked cells back and swap the sprites in one go: a full background reload takes several frames,
             * uncovering the box column by column while its in-front corner pieces linger. */
            pce_panel_column=story_column;pce_panel_restore=story_y;overlay_call(0x7b,play_draw);
        } else if(pce_metrics.stage==2)overlay_call(0x77,race_unbox);   /* the sky cells the box and the text covered, and the road's wrap copies, go back */
        else if(pce_metrics.stage==6){video_restore();overlay_call(0x6e,story_graphics_restore);}   /* the arena's columns are read in again; the font and the dialogue palette go back */
        else video_restore();
    }
}
