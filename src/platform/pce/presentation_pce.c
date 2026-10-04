#include "presentation_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#include "play_internal.h"
#include "sprite_cache_pce.h"
#define PRESENT __attribute__((noinline,section(".ram_bank116.text")))
/* Retain the HUD while its graphics, palettes and counters are unchanged.
 * Only replay onto an empty SAT/scanline prefix, so admission is identical. */
extern uint16_t warm_ids[7];extern uint8_t warm_slots[7],warm_count;
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_count,sat_page,sprite_exact,sprite_occupancy[240];
extern uint8_t sprite_last_free,fg_entered;
static vdc_sprite_t hud_sat[32];
static uint8_t hud_lines[64],hud_slots[3],hud_count,hud_exact,hud_ready;
static uint16_t hud_ids[3];
static uint8_t hud_hero,hud_hp,hud_lives,hud_powers;
extern volatile uint16_t hud_copy_src,hud_copy_dst,hud_copy_len;
extern void hud_copy(void);
extern volatile uint8_t hud_copy_opcode;
PCE_FLOW static void hud_transfer(void *dst,const void *src,uint16_t n) {
    hud_copy_opcode=0x73;
    hud_copy_src=(uint16_t)src;hud_copy_dst=(uint16_t)dst;hud_copy_len=n;hud_copy();
}
PCE_FLOW void presentation_frame(void) {
    const uint16_t *base=pce_present_base[pce_metrics.stage-1];
    if(!base[0])return;
    uint8_t hp=pce_metrics.hp>3?3:pce_metrics.hp;
    bool empty=!sat_count;
    uint8_t lines=sprite_exact?64:32;
    if(empty&&fg_entered)for(uint8_t i=0;i<lines;++i)if(sprite_occupancy[i]){empty=false;break;}
    bool hit=empty&&hud_ready&&hud_exact==sprite_exact&&hud_hero==pce_control.hero&&hud_hp==hp&&
        hud_lives==pce_campaign.lives&&hud_powers==pce_campaign.powers;
    if(hit)for(uint8_t i=0;i<3;++i)if(sprite_ids[hud_slots[i]]!=hud_ids[i]){hit=false;break;}
    if(hit) {
        hud_transfer(sat[sat_page],hud_sat,(uint16_t)hud_count*8);
        hud_transfer(sprite_occupancy,hud_lines,lines);
        sprite_last_free=0;
        sat_count=hud_count;
        for(uint8_t i=0;i<3;++i)sprite_used[hud_slots[i]]=1;
        return;
    }
    uint16_t ids[3]={base[1]+pce_campaign.lives%10,base[1]+pce_campaign.powers%10,
        base[0]+pce_control.hero*4+hp};
    /* A changed HUD icon (14 pieces = 4 contiguous pages) must find room beside the herd's reserved pages: the retained
     * firing poses give theirs up for it (the herd re-warms them over the next frames), or the icon would be refused. */
    if(herd_on)for(uint8_t i=0;i<warm_count;++i)if(sprite_ids[warm_slots[i]]==warm_ids[i])sprite_pinned[warm_slots[i]]=0;
    bool ok=video_sprite(ids[0],29,14,false,16);
    ok=video_sprite(ids[1],60,14,false,16)&&ok;
    ok=video_sprite(ids[2],0,0,false,16)&&ok;
    hud_ready=0;
    if(empty&&ok&&sat_count&&sat_count<=32) {
        for(uint8_t i=0;i<3;++i){hud_ids[i]=ids[i];hud_slots[i]=sprite_slot_of[ids[i]];}
        hud_hero=pce_control.hero;hud_hp=hp;hud_lives=pce_campaign.lives;hud_powers=pce_campaign.powers;
        hud_count=sat_count;hud_exact=sprite_exact;
        hud_transfer(hud_sat,sat[sat_page],(uint16_t)hud_count*8);
        hud_transfer(hud_lines,sprite_occupancy,lines);hud_ready=1;
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
            else if(kind==2) {
                if(a->type==8||a->type==9) {
                    /* the kneeler crouches and, from the 24th step of a throw, plays its three throwing cells */
                    uint8_t throw=a->timer>=24?(a->timer-24)/6:3;
                    id=base+(throw<3?41+throw:40);
                } else {
                    static const uint8_t pose[8]={0,1,2,1,0,3,4,3};   /* the standing aim cells: level, up-diagonal, up, down-diagonal, down */
                    uint8_t p=pose[a->aim&7];
                    id=base+34+(p==0&&a->anim?5:p);
                }
            }
        }
        if(a->type==28)id+=a->hp==2?0:a->hp==1?1:2+(a->anim>>2)%6;   /* the blue Outrider: stand, alarm, run */
        video_sprite_optional(id,a->b.x-camera,a->b.y-16,a->flip,16);   /* a refused draw is skipped for the frame, never a removal */
    }
}
void presentation_draw(void) {overlay_call(0x6e,presentation_frame);video_front_mark();}

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
