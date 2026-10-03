#include "campaign_pce.h"
#include "overlay_pce.h"
#include "arcade_pce.h"
#include "video_pce.h"
#include "presentation_pce.h"
#define STORY_CODE __attribute__((noinline,section(".ram_bank113.text")))
static uint32_t story_address;
static uint8_t page_count;
static char story_text[256];
STORY_CODE static uint32_t pointer(uint32_t a) {
    uint32_t p;arcade_read(2,a,&p,4);return p;
}
STORY_CODE static void draw(void) {
    uint32_t a=pointer(story_address+1+(uint16_t)pce_campaign.page*4);
    uint16_t avatar;arcade_read(2,a,&avatar,2);
    arcade_read(2,a+2,story_text,sizeof story_text);story_text[255]=0;
    uint8_t y=pce_metrics.stage==2?5:20;
    video_panel(4,y,27,6);
    video_sat_begin();
    if(avatar!=65535)video_sprite(avatar,8,y*8-8,false,16);
    video_sat_end();
    char *line=story_text;
    for(uint8_t row=0;row<4&&*line;++row) {
        char *end=line;while(*end&&*end!='\n')++end;
        bool more=*end!=0;*end=0;video_text(5,y+1+row,line);line=end+more;
    }
    video_text(25,y+5,"I/II >");
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
    if(pce_campaign.timer<12||!(pce_control.pressed&(KEY_1|KEY_2)))return;
    pce_campaign.timer=0;
    if(++pce_campaign.page<page_count)draw();
    else {pce_campaign.state=CAM_PLAY;video_restore();}
}
