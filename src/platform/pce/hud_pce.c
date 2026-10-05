#include "campaign_pce.h"
#include "video_pce.h"
#include "overlay_pce.h"
#include "assets.h"
#include "hud_pce.h"
#undef PCE_HUD
#define PCE_HUD __attribute__((noinline,minsize,section(".ram_bank124.text")))   /* (the cockpit HUD is not time-critical; the bank is full) */
/* Sprite HUDs of the stages that have none of the platform HUD's own: pre-rendered pieces (tools/pce/hudart.py) placed
 * at fixed spots, so nothing is drawn on background cells (those left black boxes on the art). */
Hud7 hud7;
/* in the race HUD bank ($7c): the HUD bank ($7c) has no room left for it */
#define HUD7_CODE __attribute__((noinline,section(".ram_bank124.text")))
HUD7_CODE static void put7(uint16_t offset,int16_t x,int16_t y) {
    video_sprite_optional(pce_hud_base[6]+offset,x,y,false,16);
}
/* The space flight's HUD is one narrow column at the top left (shield cells, lives, gun power, torpedoes, hero-power stars; one
 * sprite on any scanline) and, against the cruiser, a vertical hull gauge at the right edge: pieces made at build time
 * (tools/pce/hudart.py space_hud), pitched so that the whole column ends at y 90. */
HUD7_CODE void hud7_draw(void) {
    const Hud7 *h=&hud7;
    uint8_t hp=h->hp>4?4:h->hp;
    bool blink=hp==1&&((h->clock>>3)&1);
    uint8_t c0=hp>0&&!blink,c1=hp>1,c2=hp>2,c3=hp>3;
    put7(PCE_H7_CELLS_TOP_0+(c3|(c2<<1)),2,4);
    put7(PCE_H7_CELLS_BOT_0+(c1|(c0<<1)),2,22);
    put7(PCE_H7_LIFE_0+(h->lives>9?9:h->lives),2,42);
    put7(PCE_H7_PWR_0+(h->power>3?3:h->power),2,54);
    put7(PCE_H7_TRP_0+(h->bombs>5?5:h->bombs),2,66);
    put7(PCE_H7_SPC_0+(h->items>2?2:h->items),2,78);
    if(h->boss_on) {
        /* 64 dots for 1200 hull points (4 / 75 each), as four 16-dot segments from the bottom */
        uint16_t fill=(uint16_t)(h->boss_hp>1200?1200:h->boss_hp)*4/75;
        for(uint8_t i=0;i<4;++i) {
            uint16_t part=fill>(uint16_t)i*16?fill-(uint16_t)i*16:0;
            put7(PCE_H7_HULL_0+(part>16?16:part),240,8+(3-i)*16);
        }
    }
}
