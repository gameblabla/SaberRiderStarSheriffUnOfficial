#include "play_internal.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "loader_pce.h"
#include "arcade_pce.h"
#include "sprite_cache_pce.h"
extern uint8_t buffer[2048];
/* The two flying bosses of the platform stages, on the source game's timelines (enemies.c update_boss for the level-1
 * gunship, night.c for the Hyperjumper), in whole 1/60 s steps; positions are the sprite's centre in world px.
 *
 * Gunship (stages 1 and 5), phases: 0 far pass (right to left, 6.5 px a step, behind the scenery), 1 mid pass (left to
 * right), 2 sweeps: flies in at 2.3 px a step to the right edge, holds 80 steps spraying lasers down and across,
 * leaves on the far side, comes back from the other one - the only phase it takes hits in. Its rider covers the other
 * side, falling silent for steps 160-184 of each round. It hurts only with its lasers.
 * Hyperjumper (stages 3 and 4), phases: 0 far pass, 1 gap (1.3 s), 2 mid pass, 3 gap (1 s), 4 side in, 5 side hold,
 * 6 side out, 7 nose peeks, 8 low pass (jump it or slide under it), 9 drops in, 10 fires straight down at the hero,
 * 11 leaves upward, then the other side. It hurts on contact with its hull and takes hits throughout 4-11.
 * Phase 12 (both): the wreck falls and burns for 228 steps, then the stage clears. */
#define BOSS_CODE PCE_X1   /* $76: the CD buffer's bank (pce_config.h); nothing here calls a loader */
#define BOSS_DRAW __attribute__((noinline,section(".ram_bank109.text")))   /* $6d: the race road's bank, idle on the platform stages ($79 is full) */
int16_t boss_x,boss_y;
uint8_t boss_phase,boss_flash,boss_max;
uint16_t boss_time;
static uint8_t boss_dir,boss_cd,boss_rcd,boss_fx,boss_cycle;
static uint16_t boss_hold,boss_clock;
static int16_t boss_vy;
static uint16_t rng=0x1D2B;
BOSS_CODE static uint8_t rnd(void) {
    uint8_t carry=rng&1;rng>>=1;if(carry)rng^=0xB400;
    return (uint8_t)(rng^(rng>>8));
}
BOSS_CODE static int16_t bob(uint16_t t) {uint8_t n=t%66;n=n<33?n:66-n;return (int16_t)((n*3)>>4)-3;}
BOSS_CODE static void bstep(int16_t v) {
    int16_t sum=(int16_t)boss_fx+v;
    boss_x+=sum>>8;boss_fx=sum;
}
BOSS_CODE static void bshoot(int16_t x,int16_t y,int16_t vx,int16_t vy) {
    uint8_t live=0;
    for(uint8_t k=0;k<NSHOTS;++k)if(shots[k].active&&shots[k].enemy)++live;
    if(live>=NSHOTS-2)return;
    for(uint8_t k=0;k<NSHOTS;++k)if(!shots[k].active){shots[k]=(Shot){x,y,vx,vy,1,3,0,0,0};return;}
}
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page,sat_count,sprite_line_lo,sprite_line_hi,sprite_line_ok;
extern void sprite_lines_reserve(void),sprite_lines_release(void);
static uint8_t hull_count,hull_ready,hull_level,hull_seen_full,rider_low;
static uint32_t hull_patterns;   /* the pattern set in VRAM: poses of one ship share it, so only a change of ship reloads it */   /* hull_level: 0 full size, 1 and 2 the mid and far passes */
static int16_t hull_parts[28][3];
PCE_MISSION static void hull_load(void) {
    uint32_t record[3];uint16_t bytes;uint8_t colors[32];
    uint32_t address=pce_boss_big[pce_metrics.stage-1]+(uint16_t)hull_level*15;
    arcade_read(2,address,record,12);arcade_read(2,address+12,&bytes,2);
    arcade_read(2,address+14,&hull_count,1);
    for(uint8_t p=16;p<40;++p) {
        uint8_t owner=pattern_owner[p];
        if(owner&&owner!=48) {
            sprite_ids[owner-1]=0xffff;
            for(uint8_t q=0;q<48;++q)if(pattern_owner[q]==owner)pattern_owner[q]=0;
        }
        pattern_owner[p]=48;
    }
    sprite_ids[14]=0xffff;sprite_pinned[14]=250;sprite_pinned[47]=250;
    arcade_read(2,record[0],colors,32);pce_vce_copy_palette(30,colors,1);
    arcade_read(2,record[1],hull_parts,hull_count*6);
    if(record[2]!=hull_patterns) {   /* another pose of the ship on show keeps its patterns: only the piece list changed */
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(record[2],PCE_SPR_WORD+16*256,bytes);
        hull_patterns=record[2];
    }
    hull_ready=1;
}
PCE_MISSION void boss_release(void) {
    for(uint8_t p=16;p<40;++p)pattern_owner[p]=0;
    sprite_pinned[14]=sprite_pinned[47]=0;hull_ready=0;hull_patterns=0;
}
PCE_MISSION void boss_start(void) {
    uint8_t kind=pce_campaign.boss_kind;
    hull_level=2;hull_seen_full=0;hull_patterns=0;
    overlay_call(0x6f,hull_load);
    audio_effect(12);   /* the engine pass (the source's sfx 0x13) opens the fight, and every later pass */
    boss_phase=0;boss_dir=0;rider_low=0;boss_time=boss_hold=boss_clock=0;boss_flash=boss_cd=boss_rcd=boss_fx=boss_cycle=0;boss_vy=0;
    boss_x=camera+(kind==1?288:296);boss_y=kind==1?48:58;
    boss_max=kind==1?66:48+12*pce_options.difficulty;
    pce_campaign.boss_hp=boss_max;
}
__attribute__((minsize)) BOSS_CODE static void horse_step(void) {
    int16_t cam=camera;
    boss_y=48+bob(boss_clock);
    switch(boss_phase) {
    case 0:   /* the far-layer pass, leaves at 6.5 px a step */
        bstep(-1664);
        if(boss_x+350<cam){boss_phase=1;boss_x=cam-820;boss_fx=0;audio_effect(12);}
        break;
    case 1:   /* the mid-layer pass, left to right */
        bstep(1664);
        if(boss_x-230>cam+256){boss_phase=2;boss_dir=0;boss_x=cam+256+280;boss_hold=1;boss_fx=0;}
        break;
    default: {
        bool fire=false,move=false;
        if(boss_hold>80) {
            move=true;++boss_hold;
            if(boss_dir?boss_x-280>cam+256:boss_x+180<cam){boss_dir^=1;boss_hold=1;}
        } else if(boss_hold<2&&(boss_dir?boss_x<=cam+128:boss_x>=cam+128)) {
            if(boss_dir?boss_x<cam+32:boss_x>cam+224){move=true;fire=true;}else ++boss_hold;
        } else {++boss_hold;fire=true;}
        if(move)bstep(boss_dir?597:-597);
        int16_t dx=boss_x-cam;
        if(dx>-64&&dx<256) {
            bool gun=fire&&!boss_cd,pilot=!boss_rcd&&!(boss_hold>=160&&boss_hold<=184);
            if(gun){bshoot(boss_x+(boss_dir?90:-95),boss_y+50,boss_dir?996:-996,996);boss_cd=12;}
            if(pilot) {
                /* the rider on its back covers the other side, lower when the hero is 60 px below */
                bool low=player.y+8-boss_y>=60;
                int16_t vx=boss_dir?-1422:1422,vy=0;
                if(low){vx=boss_dir?-996:996;vy=996;}
                bshoot(boss_x+(boss_dir?(low?-75:-85):(low?70:80)),boss_y+(low?10:-25),vx,vy);boss_rcd=12;
            }
            if(gun||pilot)audio_effect(gun&&pilot?15:gun?13:14);
        }
        break; }
    }
}
BOSS_CODE static void hyper_step(void) {
    /* x is relative to the (locked) camera; v the horizontal speed this step in Q8; y is set per phase */
    int16_t x=boss_x-(int16_t)camera,v=0;
    uint16_t h=++boss_hold;
    uint8_t ph=boss_phase,next=ph;
    int16_t s=boss_dir?1:-1;
    bool p2=pce_campaign.boss_hp*2<=boss_max,fire=false;
    int16_t vx=s*996,vy=996,mx=-s*5,my=0;      /* the side gun: diagonal bolts, 3.9 px a step each way */
    switch(ph) {
    case 0:v=-1408;boss_y=58;if(x<-40)next=1;break;                       /* far pass, right to left */
    case 1:if(h>=78){next=2;x=-60;boss_dir=1;}break;
    case 2:v=1664;boss_y=44+bob(h);if(x>316){next=3;boss_dir=0;}break;              /* mid pass, left to right */
    case 3:if(h>=60){next=4;boss_y=66;x=boss_dir?-81:337;}break;          /* the side pass begins off screen */
    case 4: {                                                             /* in to one half of the arena, then hold */
        int16_t target=128-s*96,d=target-x;
        if(h<2000&&(d>3||d<-3)&&boss_hold<400)v=d>0?597:-597;
        else if(boss_hold<400){x=target;boss_hold=400;}
        boss_y=66+bob(h)/2;
        fire=x>8&&x<248;
        if(boss_hold>=400+(p2?108:80)){next=6;boss_vy=597;}
        break; }
    case 6:                                                               /* boosts away the way it faces, climbing */
        boss_vy+=37;if(boss_vy>1707)boss_vy=1707;
        v=s*boss_vy;boss_y=66-(int16_t)(h/3);
        if(boss_dir?x>329:x<-73){next=7;boss_dir^=1;s=-s;boss_y=149;x=boss_dir?-73:329;}
        break;
    case 7:x+=((boss_dir?-35:291)-x)/8;if(h>=48)next=8;break;             /* the nose peeks in, engine howling */
    case 8:v=s*(p2?1408:1237);                                            /* the low pass: jump it or slide under it */
        if(boss_dir?x>329:x<-73){next=9;boss_y=-78;x=player.x-camera;if(x<88)x=88;if(x>168)x=168;}
        break;
    case 9:boss_y+=h&1?3:2;if(boss_y>=56){boss_y=56;next=10;}break;       /* drops in facing the hero */
    case 10: {                                                            /* drifts after him, bolts straight down */
        int16_t t=player.x-camera;if(t<88)t=88;if(t>168)t=168;
        if(t>x)++x;else if(t<x)--x;
        boss_y=56+bob(h)/2;
        if(h>21&&(h-21)%(p2?93:102)<72){vx=0;vy=1280;mx=-7;my=-25;fire=true;}
        if(h>=(p2?300:252))next=11;
        break; }
    case 11:boss_y-=3;if(!(h%6))--boss_y;
        if(boss_y<-78){boss_cycle^=1;boss_dir=boss_cycle;next=3;boss_hold=59;}
        break;
    }
    boss_x=camera+x;
    if(v)bstep(v);
    if(fire&&!boss_cd){bshoot(boss_x+mx,boss_y+my,vx,vy);boss_cd=12;audio_effect(ph==10?14:13);}
    if(next!=ph){
        boss_phase=next;if(next!=3||ph!=11)boss_hold=0;
        if(next==2||next==4||next==7||next==9)audio_effect(12);   /* each pass and entrance has the engine's roar */
    }
}
static uint8_t hull_want;
__attribute__((noinline,section(".ram_bank114.text"))) static void hull_pose(void) {
    uint8_t kind=pce_campaign.boss_kind;
    /* The Hyperjumper's poses (record: 0 side, 3 front, 4 side boosting, 5/6 side firing (flash frame, then the other), 7 front firing); the flash
     * frame is the first half of each 12-step shot cycle. */
    bool shooting=boss_cd!=0,flash=boss_cd>6;   /* the gun's cycle (night.c night_draw_layer): the muzzle-flash frame after each shot, then the other */
    /* the engines lit while it moves about: in from the side, away, the low pass, the nose peeking in (night.c) */
    bool boost=boss_phase==6||boss_phase==8||(boss_phase==7&&(boss_hold&2))||(boss_phase==4&&boss_hold<400);
    hull_want=kind==1?(boss_phase>=2?rider_low?3:0:boss_phase==1?1:2)
        :(boss_phase>=9&&boss_phase<=11)?(shooting&&flash?7:3)
        :hull_seen_full?(shooting?(flash?5:6):boost?4:0):boss_phase>=2?1:2;
}
__attribute__((minsize)) BOSS_CODE void boss_tick(void) {
    uint8_t kind=pce_campaign.boss_kind;
    ++boss_clock;if(boss_flash)--boss_flash;if(boss_cd)--boss_cd;if(boss_rcd)--boss_rcd;
    if(boss_phase==12) {
        /* the wreck drops, jitters sideways and burns; the stage clears when the clock runs out */
        boss_vy+=34;boss_y+=boss_vy>>8;boss_x+=(int16_t)(rnd()%3)-2;
        if(!(boss_time&7)&&boss_time<64)audio_effect(16);
        if(boss_time==8)audio_effect(17);
        if(++boss_time>=228){pce_campaign.boss_hp=0;}
        return;
    }
    if(kind==1)horse_step();else hyper_step();
    /* the ship grows as it comes in from the distance: its far and mid passes use smaller hulls, the fight the full one */
    if(boss_phase>=4)hull_seen_full=1;
    /* The Hyperjumper's front pose (phases 9-11: drops in facing the hero, fires down, leaves upward) is the fourth record, in
     * Arcade RAM like the others: it replaces the side hull while the ship is off screen above (phase 8 -> 9) and goes back
     * the same way (11 -> 3). The wreck keeps the pose it was hit in. */
    /* The gunship's rider lowers its gun (record 3: the same hull with the gun at 45 degrees) when the hero is 60 px below, and
     * raises it again at 48 px, so the hull is not reloaded every time the hero bobs about the threshold. */
    int16_t below=player.y+8-boss_y;
    if(below>=60)rider_low=1;else if(below<48)rider_low=0;
    overlay_call(0x72,hull_pose);uint8_t want=hull_want;   /* which hull record (boss_pce.c hull_pose, in the cockpit bank: this one is full) */
    if(want!=hull_level){hull_level=want;hull_ready=0;}
    uint8_t vulnerable=kind==1?boss_phase==2:boss_phase>=4&&boss_phase<=11;
    if(!vulnerable)return;
    /* the ship's hull hurts on contact (the gunship has only its lasers) */
    if(kind==2&&!safe_timer) {
        int16_t dx=player.x-boss_x,top=player.y+(slide_time?8:-23),bottom=player.y+23;
        if(dx>-52&&dx<52&&bottom>boss_y-8&&top<boss_y+33){safe_timer=120;campaign_hurt();}
    }
    for(uint8_t k=0;k<NSHOTS;++k) {
        Shot *s=&shots[k];if(!s->active||s->enemy)continue;
        int16_t dx=s->x-boss_x,dy=s->y-boss_y,rx=kind==1?42:48;
        if(dx>-rx&&dx<rx&&dy>(kind==1?-15:-26)&&dy<(kind==1?27:34)) {
            s->active=0;audio_effect(22);   /* (the PSG zap, no DDA) */
            if(kind==1&&boss_flash)continue;   /* the gunship ignores a shot landing during its 4-step flash */
            boss_flash=kind==1?4:5;
            if(pce_campaign.boss_hp)--pce_campaign.boss_hp;
            if(!pce_campaign.boss_hp) {
                /* hit points are kept at 1 while it burns out; combat_tick sees 0 when the wreck is gone */
                pce_campaign.boss_hp=1;boss_phase=12;boss_time=0;boss_vy=0;
                for(uint8_t j=0;j<NSHOTS;++j)if(shots[j].enemy)shots[j].active=0;
                break;
            }
        }
    }
}
static bool hull_flip,hull_white;
/* The hull's 32x32 pieces: the sprite-allocation bank ($74) has the room, the race core's bank is full. */
__attribute__((noinline,section(".ram_bank116.text"))) static void hull_body(void) {
    bool flip=hull_flip;
    if(!hull_ready)overlay_call(0x6f,hull_load);
    sprite_pinned[14]=sprite_pinned[47]=250;
    for(uint8_t k=0;k<hull_count;++k) {
        int16_t dx=hull_parts[k][0],y=boss_y+hull_parts[k][1];   /* the hull's anchor sits at boss_y, where the muzzles below and the hit boxes assume it (it was drawn 16 px high, cutting the rider off at the top) */
        if(flip)dx=-dx-32;
        bool piece_flip=flip^(hull_parts[k][2]<0);   /* bit 15: the right half of a symmetric hull is its left half mirrored */
        int16_t x=boss_x-camera+dx;
        if(x<=-32||x>=256||y<=-32||y>=224)continue;
        sprite_line_lo=y<0?0:y;sprite_line_hi=y+32>224?224:y+32;
        sprite_lines_reserve();if(!sprite_line_ok)continue;
        sprite_lines_reserve();if(!sprite_line_ok){sprite_lines_release();continue;}
        if(sat_count>=64){sprite_lines_release();sprite_lines_release();continue;}
        sat[sat_page][sat_count++]=(vdc_sprite_t){y+64,x+32,
            ((PCE_SPR_WORD+16*256)>>5)+(hull_parts[k][2]&0x7fff)*2,
            VDC_SPRITE_FG|14|VDC_SPRITE_WIDTH_32|VDC_SPRITE_HEIGHT_32|(piece_flip?VDC_SPRITE_FLIP_X:0)};
    }
}
BOSS_DRAW static void hull(bool flip) {hull_flip=flip;overlay_call(0x74,hull_body);}
BOSS_DRAW void boss_draw(void) {
    bool flip=pce_campaign.boss_kind==2&&(hull_level==3||hull_level==7)?false:boss_phase==1?true:boss_dir;   /* the Hyperjumper's front pose is not mirrored */
    bool white=boss_flash!=0&&boss_phase!=12;   /* a hit blanks the whole hull white (palette 30 is the hull's alone); the colours come back with the hull's load */
    if(hull_white&&!white)hull_ready=0;
    hull_white=white;
    if(boss_phase==12) {
        if(boss_y<250&&(boss_time&1))hull(flip);
        for(uint8_t k=0;k<2;++k) {
            uint8_t a=(uint8_t)(frame*29+k*71+boss_time*13),b=(uint8_t)(frame*53+k*37+boss_time*7);
            video_sprite_optional(38,boss_x-camera+(int16_t)(a%48)-24,boss_y-16+(int16_t)(b%40)-20,false,16);
        }
        return;
    }
    hull(flip);
    if(white){for(uint8_t i=0;i<16;++i)((uint16_t*)buffer)[i]=0x1ff;vce_copy(30,buffer,1);}
}
