#include "play_internal.h"
#include "sprite_cache_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
/* The robot-horse herd (level 1's stampede) at the source's full size. A frame is 128x80, drawn as VDC big sprite
 * cells: four columns of a 32x64 and a 32x16 sprite (8 SAT entries a horse, not 33 pieces). The frame patterns are
 * streamed from the scene into pages of the sprite cache that the herd reserves for its length, into one of two
 * buffers so the frame on screen is never overwritten while it is displayed. */
#define HERD_CODE __attribute__((noinline,section(".ram_bank111.text")))
extern uint8_t sat_count,sat_page;
extern vdc_sprite_t sat[2][64];
extern uint8_t sprite_line_lo,sprite_line_hi,sprite_line_ok;
extern void sprite_lines_reserve(void),sprite_lines_release(void);
uint8_t herd_on;
int16_t herd_y;
static uint8_t shown,cur;
#define BUFFER_PAGE(b) (28+(b)*10)
HERD_CODE void herd_reserve(void) {
    uint8_t colors[32];
    for(uint8_t p=28;p<48;++p) {
        uint8_t owner=pattern_owner[p];
        if(owner&&owner!=48) {
            sprite_ids[owner-1]=0xffff;
            for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
        }
        pattern_owner[p]=48;
    }
    sprite_pinned[47]=250;herd_on=1;shown=0xff;cur=0;
    arcade_read(2,play_scene->horse,colors,32);
    pce_vce_copy_palette(16+15,colors,1);
}
/* The source drops a column of 12 horses at once, 99 px apart, behind the right screen edge, running at the hero at
 * 120 px/s; here five, 192 px apart, so at most two are on screen and two fit a scanline beside the hero. */
HERD_CODE void herd_spawn(void) {
    int16_t x0=camera+256+72+195;
    for(uint8_t h=0,i=0;h<5;++h) {
        while(i<8&&actors[i].active)++i;
        if(i==8)break;
        actors[i]=(Actor){.b={.x=x0+h*192,.y=herd_y+30},.active=1,.type=11,.hp=1,.flip=1};
    }
    herd_reserve();
}
/* Draw every horse; releases the pages once the last one has gone. */
HERD_CODE void herd_draw(void) {
    uint8_t live=0;
    for(uint8_t k=0;k<8;++k)if(actors[k].active&&actors[k].type==11)++live;
    if(!live) {
        for(uint8_t p=28;p<48;++p)pattern_owner[p]=0;
        sprite_pinned[47]=0;herd_on=0;return;
    }
    sprite_pinned[47]=250;
    uint8_t want=(frame>>2)%5;
    if(want!=shown) {
        cur^=1;
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(play_scene->horse+32+(uint32_t)want*5120,PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(cur)*256,5120);
        shown=want;
    }
    uint16_t code=(PCE_SPR_WORD+(uint16_t)BUFFER_PAGE(cur)*256)>>5;
    for(uint8_t k=0;k<8;++k) {
        if(!actors[k].active||actors[k].type!=11)continue;
        int16_t sx=actors[k].b.x-camera-72,sy=actors[k].b.y-48;
        for(uint8_t c=0;c<4;++c) {
            int16_t x=sx+32*c;
            if(x<=-32||x>=256)continue;
            for(uint8_t part=0;part<2;++part) {
                int16_t y=part?sy+64:sy,h=part?16:64,lo=y<0?0:y,hi=y+h>224?224:y+h;
                if(sat_count>=64||hi<=lo)continue;
                sprite_line_lo=lo;sprite_line_hi=hi;
                sprite_lines_reserve();                         /* two units a line: the cell is 32 wide */
                if(!sprite_line_ok)continue;
                sprite_lines_reserve();
                if(!sprite_line_ok){sprite_lines_release();continue;}
                sat[sat_page][sat_count++]=(vdc_sprite_t){y+64,x+32,code+(part?32+2*c:8*c)*2,
                    VDC_SPRITE_FG|15|VDC_SPRITE_WIDTH_32|(part?0:VDC_SPRITE_HEIGHT_64)};
            }
        }
    }
}
