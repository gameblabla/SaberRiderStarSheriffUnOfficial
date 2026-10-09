#include "presentation_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "overlay_pce.h"
#include "campaign_pce.h"
#include "play_internal.h"
#include "sprite_cache_pce.h"
#include "sgx_pce.h"
#include <string.h>
#define PRESENT __attribute__((noinline,section(".ram_bank116.text")))
#ifdef PCE_SGX
#define PLATFORM_DRAW __attribute__((noinline,section(".ram_bank119.text")))
#else
#define PLATFORM_DRAW PRESENT
#endif
#define PANEL __attribute__((noinline,section(".ram_bank115.text")))
/* Retain the HUD while its graphics, palettes and counters are unchanged.
 * Only replay onto an empty SAT/scanline prefix, so admission is identical. */
extern uint16_t warm_ids[7];extern uint8_t warm_slots[7],warm_count;
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_count,sat_page,sprite_exact,sprite_occupancy[240];
extern uint8_t sprite_last_free,sprite_last_lo,fg_entered,actors_mode;
extern uint8_t buffer[2048];
static vdc_sprite_t hud_sat[32];
static uint8_t hud_lines[64],hud_slots[3],hud_count,hud_exact,hud_ready;
static uint16_t hud_ids[3];
static uint8_t hud_hero,hud_hp,hud_lives,hud_powers;
extern volatile uint16_t hud_copy_src,hud_copy_dst,hud_copy_len;
extern void hud_copy(void);
extern volatile uint8_t hud_copy_opcode;
PCE_FLOW static void hud_transfer(void *dst,const void *src,uint16_t n) {
    hud_copy_opcode=0x73;
    hud_copy_src=(uint16_t)src;hud_copy_dst=(uint16_t)dst;hud_copy_len=n;
    overlay_call(0x72,hud_copy);
}
/* Keep platform actors visible when one VDC's independent sprite budget fills.
 * Actor slots retain their last successful VDC for the current stage. */
#ifdef PCE_SGX
PLATFORM_DRAW static bool split_vdc0_draw(uint16_t id,int16_t x,int16_t y,bool flip,uint8_t scale) {
    uint8_t count=sat_count,last=sprite_last_free,lo=sprite_last_lo;
    sat_page=0;sat_count=pce_sgx_split_count;sprite_last_free=pce_sgx_split_last;sprite_last_lo=pce_sgx_split_last_lo;
    overlay_call(0x6e,pce_sgx_budget_body);
    bool admitted=video_sprite_optional(id,x,y,flip,scale);
    pce_sgx_split_count=sat_count;pce_sgx_split_last=sprite_last_free;pce_sgx_split_last_lo=sprite_last_lo;
    sat_page=1;sat_count=count;sprite_last_free=last;sprite_last_lo=lo;
    overlay_call(0x6e,pce_sgx_budget_body);
    return admitted;
}
#endif
PLATFORM_DRAW bool pce_sgx_split_sprite_optional(uint8_t owner,uint16_t id,int16_t x,int16_t y,bool flip,uint8_t scale) {
#ifdef PCE_SGX
    if(pce_sgx_split_active&&sat_page==1) {
        /* Rear scenery must never retry on the foreground VDC. */
        if(actors_mode==1 && owner<8)return video_sprite_optional(id,x,y,flip,scale);
        bool admitted;
        if(owner<8&&pce_sgx_actor_plane[owner]==0) {
            admitted=split_vdc0_draw(id,x,y,flip,scale);
            if(admitted)goto split_done;
            --pce_metrics.dropped_cosmetic;
            admitted=video_sprite_optional(id,x,y,flip,scale);
            if(admitted)pce_sgx_actor_plane[owner]=1;
        } else {
            admitted=video_sprite_optional(id,x,y,flip,scale);
            if(admitted) {
                if(owner<8)pce_sgx_actor_plane[owner]=1;
            } else {
                --pce_metrics.dropped_cosmetic;
                admitted=split_vdc0_draw(id,x,y,flip,scale);
                if(admitted&&owner<8)pce_sgx_actor_plane[owner]=0;
            }
        }
split_done:
        return admitted;
    }
#else
    (void)owner;
#endif
    return video_sprite_optional(id,x,y,flip,scale);
}
#ifdef PCE_SGX
/* Projectile callers live in another overlay. Marshal through fixed renderer
 * arguments before entering the actor bank, including its VDC0 retry path. */
PLATFORM_DRAW void pce_sgx_projectile_body(void) {
    extern uint16_t sprite_emit_id;
    extern int16_t sprite_emit_x,sprite_emit_y;
    extern uint8_t sprite_emit_flip;
    pce_control.ok=pce_sgx_split_sprite_optional(255,sprite_emit_id,
        sprite_emit_x,sprite_emit_y,sprite_emit_flip!=0,16);
}
#endif
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
uint8_t actors_mode;   /* 0 every actor (the dialogue's redraw), 1 only the scenery props, 2 only the fighters (play_pce.c draws the props first, see there) */
PLATFORM_DRAW void actors_draw(void) {
    uint8_t stage=pce_metrics.stage-1;
    /* Scenery props (types 12-27: the saloon doors...) come after the fighters in the SAT, so they
     * stay behind every fighter; where the SAT or a scanline is full they are what gives way. */
    for(uint8_t pass=actors_mode==1;pass<(actors_mode==2?1:2);++pass)
    for(uint8_t actor_index=0;actor_index<8;++actor_index) {
        Actor *a=&actors[actor_index];
        if(!a->active||(a->type>=12&&a->type<=27)!=pass)continue;
        uint16_t id=pce_actor_ids[a->type];
        if(id==255)continue;
        bool fighter=id>=39&&id<=41;
        if(a->type>=30&&a->type<=31) {
            /* the shield sniper (enemy_base + 44..59): shield up, the panel burning away over 18 steps, then bare; dead with or without it */
            uint16_t base=pce_enemy_base[stage];
            if(a->dead)id=base+(a->mode&4?54:48)+(a->dead-1)/4;
            else if(a->mode&4)id=base+(a->aim>12?45:a->aim>6?46:47);
            else id=base+44;
        } else if(fighter) {
            /* walker 0 / grunt 1 / sniper 2: run frames, then six death frames */
            uint16_t base=pce_enemy_base[stage],kind=id-39;
            bool brown=a->type==5;   /* the brown grunt: the sniper's body, its own run and fall cells (appended: enemy_base + 68..) */
            if(a->dead)id=base+(kind==0?6:kind==1&&!brown?18:24)+(a->dead-1)/4;
            else if(a->mode&8&&kind==1)id=base+76+(brown?2:0)+((frame>>3)&1);   /* dropping in from its spawn point: anim 0x34 */
            else if(!(a->b.coll&4)&&kind<2)id=brown?base+74+((frame>>3)&1):base+30+kind*2+((frame>>3)&1);   /* dropping: the fall cells, not a frozen run frame */
            else if(a->b.vx&&kind<2)id=brown?base+68+(a->anim>>2)%6:base+kind*12+(a->anim>>2)%6;
            else if(brown)id=base+34+(a->timer>=16?5:0);   /* standing to fire: the blue body's level aim (then recoil), not the tan grunt's stand cell it shares an id with */
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
        int16_t sx=a->b.x-camera;
        /* the boss arena's tower sniper (type 31, the brown watchtower) is drawn 5 px higher (Y - 5) */
        int16_t sy=a->b.y-16-(a->type==31?5:0);
        /* a refused draw is skipped for the frame, never a removal; a fighter refused on screen holds the filler spawns back */
        if(!pce_sgx_split_sprite_optional(actor_index,id,sx,sy,a->flip,16)&&fighter&&sx>-24&&sx<272)enemy_pressure=40;
    }
}
void presentation_draw(void) {overlay_call(0x6e,presentation_frame);video_front_mark();}

/* Full-address block copies must not target a promoted zero-page symbol. */
uint16_t pce_panel_column __attribute__((section(".bss.pce_panel_column")));
static uint8_t panel_x,panel_y,panel_w,panel_h;
PANEL static void panel_draw(void) {
    uint8_t x=panel_x,y=panel_y,w=panel_w,h=panel_h;
    extern volatile uint16_t pce_scroll_x;
    uint16_t scroll_col=pce_scroll_x>>3;
    for(uint8_t row=0;row<h;++row)for(uint8_t col=0;col<w;++col) {
        uint16_t address=pce_raster_enabled?(uint16_t)(48+y+row)*128+x+col:
            (uint16_t)(y+row)*64+((scroll_col+x+col)&63);
        video_vdc(0,address);video_vdc(2,0xf000+(PCE_FONT_WORD>>4));
    }
}

/* Blank BG cells (the font's space glyph) where a sprite panel sits behind the text layer. */
void video_panel(uint8_t x,uint8_t y,uint8_t w,uint8_t h) {
    panel_x=x;panel_y=y;panel_w=w;panel_h=h;overlay_call(0x73,panel_draw);
}

/* Put back the background cells a dialogue panel blanked and typed over (rows y..y+5, screen columns 3..30) from the
 * column cache and the map, using the column saved when the panel opened. A full reload takes several frames and uncovers the box column by column while its
 * in-front corner pieces linger. Two steps so the cells can land in the frame the sprites leave: _prepare does the
 * slow Arcade reads into `buffer` (28 columns x 6 words); _apply, called just after a VBlank, only writes them
 * (address increment 64, one address a column) and finishes before the beam reaches the panel. */
extern uint16_t columns[33][30];
extern const PceScene *video_scene_ptr;
extern uint8_t buffer[2048];
#ifdef PCE_SGX
PRESENT
#else
PCE_HUD
#endif
static void panel_prepare_body(void) {
    const PceScene *sc=video_scene_ptr;
    uint8_t y=panel_y;
    uint16_t *words=(uint16_t*)buffer;
    uint32_t map=sc->map;
#ifdef PCE_SGX
    if(pce_sgx_gameplay()&&sc->sgx_map)map=sc->sgx_map;
#endif
    for(uint8_t x=3;x<31;++x) {
        uint16_t world=pce_panel_column+x;
        uint8_t raw[18];
        arcade_read(1,map+(uint32_t)(world%sc->cols)*90+(uint16_t)y*3,raw,18);
        for(uint8_t row=0;row<6;++row)
            words[(x-3)*6+row]=(PCE_BG_WORD>>4)+columns[world%33][y+row]+((uint16_t)raw[row*3+2]<<12);
    }
}
PCE_CODE static void panel_apply_body(void) {
    uint16_t *words=(uint16_t*)buffer;
    uint8_t y=panel_y;
    uint16_t control=*(volatile uint16_t *)0x20f3;
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = (control>>8)|0x10;
    for(uint8_t x=3;x<31;++x) {
        uint16_t address=(uint16_t)y*64+((pce_panel_column+x)&63);
        *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
        *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
        for(uint8_t row=0;row<6;++row){uint16_t w=words[(x-3)*6+row];*IO_VDC_DATA_LO=w;*IO_VDC_DATA_HI=w>>8;}
    }
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = control>>8;
    pce_vdc_index=5;
    pce_cpu_irq_enable();
}
/* A dialogue's panel cells as platform_box (story_pce.c) leaves them in buffer+1024, row by row; the four 2x2 corner blocks stay with the scenery under the
 * sprite corners. Written like panel_apply_body, right after the VBlank that brings the sprite corners, so the corners and the panel appear in the same frame
 * (one cell at a time through video_vdc ran past the end of the VBlank, and the corners showed alone for a frame). */
__attribute__((noinline,section(".ram_bank115.text"))) static void cells_apply_body(void) {   /* $73: $74 is full */
    const uint16_t *cells=(const uint16_t*)(buffer+1024);
    uint8_t y=panel_y;
    uint16_t control=*(volatile uint16_t *)0x20f3;
    pce_cpu_irq_disable();
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = (control>>8)|0x10;
    for(uint8_t x=0;x<28;++x) {
        uint8_t first=(x<2||x>=26)?2:0,last=first?4:6;
        uint16_t address=(uint16_t)(y+first)*64+((pce_panel_column+3+x)&63);
        *(volatile uint8_t *)0x20f7 = 0;*IO_VDC_INDEX = 0;*IO_VDC_DATA_LO = address;*IO_VDC_DATA_HI = address>>8;
        *(volatile uint8_t *)0x20f7 = 2;*IO_VDC_INDEX = 2;
        for(uint8_t row=first;row<last;++row){uint16_t w=cells[(uint16_t)row*28+x];*IO_VDC_DATA_LO=w;*IO_VDC_DATA_HI=w>>8;}
    }
    *(volatile uint8_t *)0x20f7 = 5;*IO_VDC_INDEX = 5;*IO_VDC_DATA_LO = control;*IO_VDC_DATA_HI = control>>8;
    pce_vdc_index=5;
    pce_cpu_irq_enable();
}
void video_cells_apply(uint8_t y,uint16_t column) {panel_y=y;pce_panel_column=column;overlay_call(0x73,cells_apply_body);}
void video_panel_restore_prepare(uint8_t y) {
    panel_y=y;
#ifdef PCE_SGX
    overlay_call(0x74,panel_prepare_body);
#else
    overlay_call(0x7c,panel_prepare_body);
#endif
}
void video_panel_restore_apply(void) {
    overlay_call(0x69,panel_apply_body);
}
