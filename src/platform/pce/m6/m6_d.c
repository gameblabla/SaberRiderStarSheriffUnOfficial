#pragma clang section text=".ram_bank131.text" rodata=".ram_bank131.rodata" data=".ram_bank131.data" bss=".ram_bank131.bss"
#define M6_SECTION ".ram_bank131.rodata"
#include "m6_common.h"
#include <string.h>
#include "sgx_pce.h"
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page,sat_count;
extern uint8_t sprite_occupancy[240];
void sprite_lines_clear(void);
#include "m6.h"
#define M6_HORIZON 128
#define BIG_FLAG 0xf000
extern volatile uint16_t pce_scroll_y;
extern uint8_t sprite_line_ok,sprite_line_lo,sprite_line_hi;
void sprite_lines_reserve(void),sprite_lines_release(void);
/* Ramrod's bolts: three frames made in advance (tools/pce/build_assets.py m6_bolts: very near 32x32, middle 16x16, far 16x16 with a small ball), kept in VRAM at $7c80 (the
 * fixed HUD ends there; m6_c.c m6_start writes them) and drawn by the picture's own emitter in the HUD's palette 31, whichever size the bolt's distance asks for. */
#define BOLT_FLAG 0xe000
#define BOLT_WORD 0x7c80
/* Ramrod's arena, the picture (the source's ramrod.c render_world / render_aim): the sky scrolls with the bearing Ramrod faces; every mech, boulder,
 * shot and burst is a sprite of the size ladder (tools/pce/build_assets.py) that its distance asks for, on the row the source's projection gives it
 * (214 / distance under the horizon), nearest first (a lower SAT slot is in front). HUD: the armour and gun-heat bars, wave, mechs left and spares,
 * the arrows for mechs out of view and the red reticle (Battlezone's); Ramrod's arm only while it punches. */
typedef struct {int16_t sx,sy;uint16_t d,id;} Item;
static Item items[36];static uint8_t nitems;
static int16_t shake_x,shake_y;
void m6_vram_body(void) {
    uint32_t address=a6.vram_address;
    uint16_t word=a6.vram_word,size=a6.vram_size;
    (void)arcade_vram(address,word,size);
    (void)arcade_vram_to(1,address,word,size);
}
/* a thing at distance d (units), bearing rel (1/16 dot from the aim), `height` px above the ground (sprite anchors are at the foot) */
static void put_at(uint16_t d,uint16_t late,int16_t rel,int16_t height,uint16_t id) {
    if(nitems>=36||d>1600)return;
    int16_t sx=128+(rel>>4),sy=M6_HORIZON+m6_row[d>>4]-height;
    if(sx<-70||sx>326)return;
    uint8_t at=nitems;
    uint16_t key=d+late;   /* the sort key: boulders go after everything else, where the sprite table's shortage is felt first */
    while(at&&items[at-1].d>key){items[at]=items[at-1];--at;}
    items[at]=(Item){sx,sy,key,id};++nitems;
}
static void put(uint16_t d,int16_t rel,int16_t height,uint16_t id) {put_at(d,0,rel,height,id);}
/* a height in 1/8 unit as pixels at distance d (with the arena's zoom, 1.6) */
static inline int16_t lift(uint16_t d,int16_t z) {
    if(d<160)d=160;   /* the rows under the horizon stop growing here (m6_row): the height stops too, or a close shot or burst climbs the screen */
    uint16_t i=d>>3;uint16_t px=((uint16_t)(z>>3)*(m6_rcp[i<200?i:200]>>2))>>6;return (int16_t)(px+(px>>1)+(px>>3));
}
/* the big mech and the arm are drawn in the world's image (m6_c.c m6_blit) */
static bool blit(uint8_t kind,uint16_t key,int16_t x,int16_t y,bool flip) {
    a6.blit_kind=kind;a6.blit_key=key;a6.blit_x=x;a6.blit_y=y;a6.blit_flip=flip;overlay_call(M6C_BANK,m6_blit);return a6.blit_ok;
}
/* a bolt (frame 0 very near, 1 middle, 2 far), its centre at (x, y): two units of every scanline for the 32-wide one */
static void bolt_draw(uint8_t frame,int16_t x,int16_t y) {
    bool big=frame==0;
    int16_t size=big?32:16;
    x-=size>>1;y-=size>>1;
    if(x<=-32||x>=256||y<=-32||y>=224||sat_count>=64)return;
    sprite_line_lo=y<0?0:y;sprite_line_hi=y+size>224?224:y+size;
    sprite_lines_reserve();if(!sprite_line_ok)return;
    if(big){sprite_lines_reserve();if(!sprite_line_ok){sprite_lines_release();return;}}
    sat[sat_page][sat_count++]=(vdc_sprite_t){y+64,x+32,(BOLT_WORD>>5)+(frame==1?0:frame==2?2:4),VDC_SPRITE_FG|15|(big?VDC_SPRITE_WIDTH_32|VDC_SPRITE_HEIGHT_32:0)};
}
#define SGX_ARENA_SPLIT 420
static bool arena_far(const Item *item) {
    return item->d>SGX_ARENA_SPLIT&&((item->id&0xf000)!=BOLT_FLAG);
}
static void draw_item(const Item *item) {
    int16_t ix=item->sx-shake_x,iy=item->sy-shake_y;
    if(item->id>=BIG_FLAG) {
        if(!blit(0,item->id&~BIG_FLAG,ix,iy,false))
            video_sprite_optional(PCE_M6_MECH+(((item->id&~BIG_FLAG)>>2)*PCE_M6_STEPS)+PCE_M6_STEPS-1,ix,iy,false,16);
    } else if(item->id>=BOLT_FLAG) bolt_draw(item->id&3,ix,iy);
    else video_sprite_optional(item->id,ix,iy,false,16);
}
void m6_draw(void) {
    /* the sky: 1344 dots to the turn, the planet ahead at the start */
    uint16_t cam=(uint16_t)((a6.cam>>4)+385);
    shake_x=a6.shake?(int16_t)((pce_ticks*37u)%(a6.shake>12?9:5))-(a6.shake>12?4:2):0;
    shake_y=a6.shake?(int16_t)((pce_ticks*53u)%(a6.shake>12?7:3))-(a6.shake>12?3:1):0;
    pce_scroll_hold=1;
    video_background(cam);
    pce_scroll_x=cam+shake_x;pce_scroll_y=shake_y;   /* (the world's sprites below move the other way: with the background, not against it) */
    a6.floor_x=cam+shake_x;a6.floor_y=shake_y;overlay_call(M6C_BANK,m6_floor);
    /* what is on screen, nearest first */
    nitems=0;
    int16_t lock_sx=0,lock_sy=0,lock_d=0,lock_hp=0;
    uint8_t *threat=a6.threat;threat[0]=threat[1]=threat[2]=0;
    bool big_taken=false;
    uint8_t nearest=255;{uint16_t best=0xffff;for(uint8_t k=0;k<3;++k){Mech6 *m=&a6.mech[k];if(m->st!=S_OFF&&m->dist<best){best=m->dist;nearest=k;}}}
    for(uint8_t k=0;k<3;++k) {
        Mech6 *m=&a6.mech[k];if(m->st==S_OFF)continue;
        uint16_t d=m->dist>>2;int16_t rel=relq(m->ang);
        if(abs16(rel)>162*16||d>1600){if(m->st!=S_DYING)threat[k]=rel<0?1:2;continue;}
        uint8_t idx=m6_size[d>>4],v=m->variant;
        if(v==2)idx=idx+2>14?14:idx+2;   /* the command mech is 1.3 times the others: two steps up the ladder */
        if(idx>=PCE_M6_STEPS&&(big_taken||k!=nearest))idx=PCE_M6_STEPS-1;   /* only the nearest takes a big step */
        uint8_t pose=(m->anim>>5)&3;
        switch(m->st){case S_AIM:case S_FIRE:pose=4;break;case S_WINDUP:pose=5;break;case S_PUNCH:pose=6;break;case S_STAGGER:case S_DYING:pose=7;break;default:break;}
        if(m->st==S_AIM||m->st==S_WINDUP||m->st==S_CHARGE)threat[k]|=4;
        if(m->st==S_DYING&&(m->dying&1))continue;
        if(idx>=PCE_M6_STEPS){big_taken=true;put(d,rel,0,BIG_FLAG|((uint16_t)(v*8+pose)<<2)|(idx-PCE_M6_STEPS));}
        else put(d,rel,0,PCE_M6_MECH+((uint16_t)v*8+pose)*PCE_M6_STEPS+idx);
        put(d+1,rel,0,PCE_M6_SHADOW+(idx>>1));
        /* the cannon charging / the fist glowing before a punch (the source's telegraphs): a ball of plasma at the hand */
        if(m->st==S_AIM||m->st==S_WINDUP) {
            uint16_t sc=m6_scq[idx];
            int16_t hx=(m->st==S_AIM?22:30)*(int16_t)(sc>>4),hy=(int16_t)(((m->st==S_AIM?92:84)*(sc>>2))>>6);   /* the hand: offsets of the source's art, scaled (hx in 1/16 dot) */
            put(d>0?d-1:0,rel+hx,hy,PCE_M6_PLASMA+((pce_ticks>>2)&1)*5+m6_fsize[d>>4]);
        }
        if(a6.lock==k&&m->st!=S_DYING){lock_sx=128+(rel>>4);lock_sy=M6_HORIZON+m6_row[d>>4]-(int16_t)(((uint16_t)140*(m6_scq[idx]>>2))>>6)-8;lock_d=d;lock_hp=v==0?m->hp:v==1?m->hp>>1:m->hp>>2;}
    }
    for(uint8_t k=0;k<14;++k) {
        Prop6 *p=&a6.prop[k];
        uint16_t d=p->dist>>2;int16_t rel=relq(p->ang);
        if(d>1500||abs16(rel)>162*16)continue;
        put_at(d,4096,rel,0,PCE_M6_ROCK_BIG+p->kind*5+m6_psize[d>>4]);
    }
    for(uint8_t k=0;k<16;++k) {
        Shot6 *s=&a6.shot[k];if(!s->life)continue;
        uint16_t d=s->dist>>2;int16_t rel=relq(s->ang);
        if(abs16(rel)>162*16)continue;
        put(d,rel,lift(d,s->z),s->enemy?PCE_M6_PLASMA+((pce_ticks>>2)&1)*5+m6_fsize[d>>4]:BOLT_FLAG|(d<420?0:d<760?1:2));
    }
    for(uint8_t k=0;k<10;++k) {
        Fx6 *e=&a6.fx[k];if(!e->dur)continue;
        uint16_t d=e->dist>>2;int16_t rel=relq(e->ang);
        if(abs16(rel)>162*16)continue;
        int8_t size=(int8_t)m6_esize[d>>4]+(int8_t)e->size-1;if(size<0)size=0;if(size>3)size=3;
        uint8_t frame=e->t/6;
        put(d>1?d-2:0,rel,lift(d,e->z),PCE_M6_EXPL+size*5+(frame>4?4:frame));
    }
    video_sat_begin();
    a6.lock_x=lock_sx-8-shake_x;a6.lock_y=lock_sy-shake_y;a6.lock_hp=lock_d?lock_hp:0;
    overlay_call(M6A_BANK,m6_hud);
    uint8_t front=sat_count;
    /* Ramrod's arm: in front of the world */
    /* Ramrod's arm, only while it punches: the source's ten frames played out, held, pulled back (the right fist is the left one mirrored) */
    if(a6.punch_t>=0) {
        int8_t t=a6.punch_t;
        int8_t fr=t<17?t*10/17:t<22?9:9-(t-22)*10/12;
        if(fr<0)fr=0;if(fr>9)fr=9;
        int16_t y=206-m6_armh[fr]+(a6.speed>>3);
        blit(1,fr,a6.punch_side?256:0,y,a6.punch_side);
    }
    /* the world, nearest first */
#ifdef PCE_SGX
    if(pce_sgx_arena_sprites()) {
        /* Mode 1 puts VDC0 sprites over VDC1 sprites. Keep the nearer half
           with the HUD and Ramrod's arm; distant objects use a second SAT
           and its independent per-scanline admission budget. */
        for(uint8_t k=0;k<nitems;++k)if(!arena_far(&items[k]))draw_item(&items[k]);
        uint8_t sat0_count=sat_count;
        memcpy(buffer,sprite_occupancy,224);
        sat_page=1;sat_count=0;sprite_lines_clear();
        for(uint8_t k=0;k<nitems;++k)if(arena_far(&items[k]))draw_item(&items[k]);
        pce_sgx_sat1_count=sat_count;
        sat_page=0;sat_count=sat0_count;
        memcpy(sprite_occupancy,buffer,224);
        overlay_call(0x78,pce_sgx_arena_sat_upload_body);
    } else
#endif
    {
        for(uint8_t k=0;k<nitems;++k)draw_item(&items[k]);
    }
    uint8_t m0=sat_count;
    overlay_call(M6A_BANK,m6_msgs);
    if(sat_count>m0&&m0>front&&sat_count-m0<=8) {   /* the messages go to the front, behind only the fixed HUD */
        uint8_t n=sat_count-m0,a=m0-front;
        vdc_sprite_t *at=sat[sat_page]+front;
        memcpy(buffer+1024,at,a*8);memcpy(buffer+1536,at+a,n*8);
        memcpy(at,buffer+1536,n*8);memcpy(at+n,buffer+1024,a*8);
    }
    video_sat_end();
    pce_metrics.player_x=(uint16_t)(a6.aim>>4);pce_metrics.player_y=a6.heat;
}
