#include "presentation_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#include "play_internal.h"
#define PRESENT __attribute__((noinline,section(".ram_bank116.text")))
PRESENT void presentation_frame(void) {
    const uint16_t *base=pce_present_base[pce_metrics.stage-1];
    if(base[0]) {
        uint8_t hp=pce_metrics.hp>3?3:pce_metrics.hp;
        video_sprite(base[1]+pce_campaign.lives%10,29,14,false,16);
        video_sprite(base[1]+pce_campaign.powers%10,60,14,false,16);
        video_sprite(base[0]+pce_control.hero*4+hp,0,0,false,16);
    }
}
/* Every live actor, optional ones last (a herd or a distant enemy that does not fit the SAT is simply not drawn). */
PRESENT void actors_draw(void) {
    uint8_t stage=pce_metrics.stage-1;
    for(uint8_t k=0;k<8;++k) if(actors[k].active) {
        Actor *a=&actors[k];uint16_t id=pce_actor_ids[a->type];
        if(id==255)continue;
        if(id>=39&&id<=41) {
            /* walker 0 / grunt 1 / sniper 2: run frames, then six death frames */
            uint16_t base=pce_enemy_base[stage],kind=id-39;
            if(a->dead)id=base+(kind==0?6:kind==1?18:24)+(a->dead-1)/4;
            else if(!(a->b.coll&4)&&kind<2)id=base+30+kind*2+((frame>>3)&1);   /* dropping: the fall cells, not a frozen run frame */
            else if(a->b.vx&&kind<2)id=base+kind*12+(a->anim>>2)%6;
        }
        if(a->type==28)id+=a->hp==2?0:a->hp==1?1:2+(a->anim>>2)%6;   /* the blue Outrider: stand, alarm, run */
        video_sprite_optional(id,a->b.x-camera,a->b.y-16,a->flip,16);   /* a refused draw is skipped for the frame, never a removal */
    }
    if(herd_on)overlay_call(0x6f,herd_draw);
}
void presentation_draw(void) {overlay_call(0x74,presentation_frame);video_front_mark();}

static uint8_t panel_x,panel_y,panel_w,panel_h;
PRESENT static void panel_draw(void) {
    uint8_t x=panel_x,y=panel_y,w=panel_w,h=panel_h;
    extern volatile uint16_t pce_scroll_x;
    for(uint8_t row=0;row<h;++row)for(uint8_t col=0;col<w;++col) {
        uint16_t address=pce_raster_enabled?(uint16_t)(48+y+row)*128+x+col:
            (uint16_t)(y+row)*64+(((pce_scroll_x>>3)+x+col)&63);
        video_vdc(0,address);video_vdc(2,0xf000+(PCE_FONT_WORD>>4));
    }
}

/* Blank BG cells (the font's space glyph) where a sprite panel sits behind the text layer. */
void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h) {
    panel_x=x;panel_y=y;panel_w=w;panel_h=h;overlay_call(0x74,panel_draw);
}

/* Put back the background cells a dialogue panel blanked and typed over (rows y..y+5, screen columns 3..30) from the
 * column cache and the map. A full reload takes several frames and uncovers the box column by column while its
 * in-front corner pieces linger. Two steps so the cells can land in the frame the sprites leave: _prepare does the
 * slow Arcade reads into `buffer` (28 columns x 6 words); _apply, called just after a VBlank, only writes them
 * (address increment 64, one address a column) and finishes before the beam reaches the panel. */
extern uint16_t columns[33][30];
extern const PceScene *video_scene_ptr;
extern uint8_t buffer[2048];
PRESENT static void panel_prepare_body(void) {
    extern volatile uint16_t pce_scroll_x;
    const PceScene *sc=video_scene_ptr;
    uint8_t y=panel_y;
    uint16_t *words=(uint16_t*)buffer;
    for(uint8_t x=3;x<31;++x) {
        uint16_t world=(pce_scroll_x>>3)+x;
        uint8_t raw[18];
        arcade_read(1,sc->map+(uint32_t)(world%sc->cols)*90+(uint16_t)y*3,raw,18);
        for(uint8_t row=0;row<6;++row)
            words[(x-3)*6+row]=(PCE_BG_WORD>>4)+columns[world%33][y+row]+((uint16_t)raw[row*3+2]<<12);
    }
}
PRESENT static void panel_apply_body(void) {
    extern volatile uint16_t pce_scroll_x;
    uint16_t *words=(uint16_t*)buffer;
    uint8_t y=panel_y;
    uint16_t control=*(volatile uint16_t *)0x20f3;
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = (control>>8)|0x10;
    for(uint8_t x=3;x<31;++x) {
        uint16_t address=(uint16_t)y*64+(((pce_scroll_x>>3)+x)&63);
        *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
        *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
        for(uint8_t row=0;row<6;++row){uint16_t w=words[(x-3)*6+row];*IO_VDC_DATA_LO=w;*IO_VDC_DATA_HI=w>>8;}
    }
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = control>>8;
    pce_cpu_irq_enable();
}
void video_panel_restore_prepare(uint8_t y) {panel_y=y;overlay_call(0x74,panel_prepare_body);}
void video_panel_restore_apply(void) {overlay_call(0x74,panel_apply_body);}

/* The same for blanking: the panel's cells (every one of the 5/3/5 column blocks of video_panel) written column by
 * column with address increment 64, right after a VBlank, so they go blank in the frame the box sprites arrive. */
PRESENT static void panel_blank_body(void) {
    extern volatile uint16_t pce_scroll_x;
    uint8_t y=panel_y;
    uint16_t control=*(volatile uint16_t *)0x20f3,blank=0xf000+(PCE_FONT_WORD>>4);
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = (control>>8)|0x10;
    for(uint8_t x=3;x<31;++x) {
        bool edge=x<5||x>28;                       /* the four 2x2 corner blocks stay: only the two middle rows go */
        uint8_t first=edge?2:0,rows=edge?2:6;
        uint16_t address=(uint16_t)(y+first)*64+(((pce_scroll_x>>3)+x)&63);
        *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
        *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
        for(uint8_t row=0;row<rows;++row){*IO_VDC_DATA_LO=blank;*IO_VDC_DATA_HI=blank>>8;}
    }
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = control>>8;
    pce_cpu_irq_enable();
}
void video_panel_blank(uint8_t y) {panel_y=y;overlay_call(0x74,panel_blank_body);}
