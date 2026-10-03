#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "presentation_pce.h"
#include "sprite_cache_pce.h"
#include <string.h>
#define STORY_CODE __attribute__((noinline,section(".ram_bank113.text")))
static uint32_t story_address;
static uint8_t page_count;
static char story_text[256];
STORY_CODE static uint32_t pointer(uint32_t a) {
    uint32_t p;arcade_read(2,a,&p,4);return p;
}
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page,sat_count;
extern volatile uint16_t pce_scroll_x;
/* Saturn-style dialogue: the box is two sprite halves in the page's colour,
 * drawn behind the background layer; the BG cells under it are blank, so the
 * Saturn-font text characters sit on top of the box. */
STORY_CODE static void draw(void) {
    uint32_t a=pointer(story_address+1+(uint16_t)pce_campaign.page*4);
    uint16_t avatar;uint8_t colour;
    arcade_read(2,a,&avatar,2);arcade_read(2,a+2,&colour,1);
    arcade_read(2,a+3,story_text,sizeof story_text);story_text[255]=0;
    uint8_t y=pce_metrics.stage==2?5:20;
    /* BG cells sit (scroll & 7) pixels left of their grid on a scrolling playfield. */
    int16_t box_x=24-(pce_raster_enabled?0:(pce_scroll_x&7)),box_y=y*8-(pce_metrics.stage!=2&&pce_metrics.stage<6?16:0);   /* platform sprites are baked 16 lines low */
    /* Blank every BG cell under the box except the four 2x2 corner blocks; the corner pieces stay in front of the
     * scenery so their rounded edges show the scenery, not a hole. */
    video_panel(5,y,24,2);video_panel(3,y+2,28,2);video_panel(5,y+4,24,2);
    video_sat_begin();
    if(avatar!=65535)video_sprite(avatar,box_x-26,box_y-8,false,16);
    uint8_t first=sat_count;
    uint16_t box=pce_dialog_base[pce_metrics.stage-1]+(colour&3)*2;
    video_sprite(box,box_x,box_y,false,16);video_sprite(box+1,box_x+112,box_y,false,16);
    uint16_t left=0xffff,top=0xffff;
    for(uint8_t k=first;k<sat_count;++k){if(sat[sat_page][k].x<left)left=sat[sat_page][k].x;if(sat[sat_page][k].y<top)top=sat[sat_page][k].y;}
    for(uint8_t k=first;k<sat_count;++k) {
        vdc_sprite_t *e=&sat[sat_page][k];
        bool corner=(e->x==left||e->x==left+208)&&(e->y==top||e->y==top+32);
        if(!corner)e->attr&=~VDC_SPRITE_FG;
    }
    video_sat_end();
    char *line=story_text;uint8_t lines=0;
    for(char *c=line;;++c){if(*c=='\n'||!*c){++lines;if(!*c)break;}}
    /* One line sits mid-box; two lines get a blank row between them. */
    uint8_t row=y+(lines==1?2:1),pitch=lines==2?2:1;
    for(;lines&&*line;--lines,row+=pitch) {
        char *end=line;while(*end&&*end!='\n')++end;
        bool more=*end!=0;*end=0;video_text(5,row,line);line=end+more;
    }
}
STORY_CODE static void arrow(bool on) {
    video_text(28,pce_metrics.stage==2?9:24,on?"\x7f":" ");
}
STORY_CODE void story_start(void) {
    uint32_t dir=pce_scenes[pce_metrics.stage-1].story;
    uint8_t count;arcade_read(2,dir,&count,1);
    if(pce_campaign.story>=count){pce_campaign.state=CAM_PLAY;return;}
    story_address=pointer(dir+1+((uint16_t)pce_control.hero*count+pce_campaign.story)*4);
    arcade_read(2,story_address,&page_count,1);
    pce_campaign.page=0;pce_campaign.state=CAM_STORY;pce_campaign.timer=0;
    video_sat_begin();video_sat_end();draw();
}
STORY_CODE void story_step(void) {
    pce_campaign.timer+=pce_control.elapsed;
    arrow(!(pce_campaign.timer&32));
    if(pce_campaign.timer<12||!(pce_control.pressed&(KEY_1|KEY_2)))return;
    pce_campaign.timer=0;
    if(++pce_campaign.page<page_count)draw();
    else {
        /* The box and avatar were cached over pattern pages that retained foreground chunks own;
         * release them and make the foreground re-admit every chunk. */
        memset(sprite_used,0,sizeof sprite_used);memset(sprite_pinned,0,sizeof sprite_pinned);
        foreground_reset();
        pce_campaign.state=CAM_PLAY;video_restore();
    }
}
