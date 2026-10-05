#pragma clang section text=".ram_bank130.text" rodata=".ram_bank130.rodata" data=".ram_bank130.data" bss=".ram_bank130.bss"
#define M6_SECTION ".ram_bank130.rodata"
#include "m6_common.h"
#include "arcade_pce.h"
#define M6_RCP_ONLY
#include "m6.h"
/* Image C: the world (ramrod.c ramrod_update and update_shots): the clock, the waves, the shots and what they hit, the lock, the bursts. */
static const uint8_t WAVE_N[3]={3,5,4},WAVE_MAX[3]={2,3,2};
static const uint16_t W_DELAY[3][8]={{0,240,600},{0,30,60,540,660},{0,60,420,780}};
static inline uint16_t rcp_of(uint16_t dist_q2) {uint16_t i=dist_q2>>5;return m6_rcp[i<200?i:200];}
extern const PceScene *video_scene_ptr;
extern volatile uint16_t pce_scroll_x,pce_scroll_y;
extern vdc_sprite_t sat[2][64];
extern uint8_t sat_page,sat_count,sprite_line_ok,sprite_line_lo,sprite_line_hi;
void sprite_lines_reserve(void),sprite_lines_release(void);
/* Ramrod's arm: the ten frames of a punch (tools/pce/build_assets.py), each its own pieces and patterns in Arcade RAM, in one of two alternating buffers of 32 patterns
 * ($2800 and $4000: the cache of background characters stops at $2800, and the dialogue's characters that share $4000 are written again when a dialogue closes: m6_a.c
 * clears arm_key then), palette 29. Cells in a row come as 32x16 pairs. */
static const uint16_t ARM_WORD[2]={0x2800,0x4000};
static uint8_t arm_shown,arm_palette;
uint8_t arm_key[2]={255,255};
static bool arm_draw(uint8_t fr,int16_t x,int16_t y,bool flip) {
    uint8_t e[10],n;uint32_t pieces,patterns;
    arcade_read(2,PCE_M6_ARMT+(uint32_t)fr*10,e,10);
    pieces=e[0]|(uint32_t)e[1]<<8|(uint32_t)e[2]<<16|(uint32_t)e[3]<<24;patterns=e[4]|(uint32_t)e[5]<<8|(uint32_t)e[6]<<16|(uint32_t)e[7]<<24;n=e[8];
    uint8_t list[6*28];
    arcade_read(2,pieces,list,(uint16_t)n*6);
    uint8_t buf=arm_key[arm_shown]==fr?arm_shown:arm_shown^1;
    if(arm_key[buf]!=fr) {
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(patterns,ARM_WORD[buf],(uint16_t)e[9]*128);
        arm_key[buf]=fr;
    }
    arm_shown=buf;
    if(!arm_palette){uint8_t pal[32];arcade_read(2,PCE_M6_ARMPAL,pal,32);vce_copy(29,pal,1);arm_palette=1;}
    uint8_t first=sat_count;
    uint16_t pattern=ARM_WORD[buf]>>5;
    for(uint8_t i=0;i<n;++i) {
        const uint8_t *q=list+i*6;
        int16_t dx=(int16_t)(q[0]|(uint16_t)q[1]<<8),dy=(int16_t)(q[2]|(uint16_t)q[3]<<8);
        bool pair=q[4];
        int16_t px=flip?x-dx-(pair?32:16):x+dx,py=y+dy;
        if(px<=-32||px>=256||py<=-16||py>=224)continue;
        if(sat_count>=64)goto refuse;
        sprite_line_lo=py<0?0:py;sprite_line_hi=py+16>224?224:py+16;
        sprite_lines_reserve();if(!sprite_line_ok)goto refuse;
        if(pair){sprite_lines_reserve();if(!sprite_line_ok){sprite_lines_release();goto refuse;}}
        sat[sat_page][sat_count++]=(vdc_sprite_t){py+64,px+32,pattern+(uint16_t)q[5]*2,VDC_SPRITE_FG|13|(pair?VDC_SPRITE_WIDTH_32:0)|(flip?VDC_SPRITE_FLIP_X:0)};
    }
    return true;
refuse:
    while(sat_count>first) {
        vdc_sprite_t *t=&sat[sat_page][--sat_count];
        int16_t py=(int16_t)t->y-64;
        sprite_line_lo=py<0?0:py;sprite_line_hi=py+16>224?224:py+16;
        sprite_lines_release();if(t->attr&VDC_SPRITE_WIDTH_32)sprite_lines_release();
    }
    return false;
}
/* The nearest mech at its four big steps: pieces of 32x32 (tools/pce/build_assets.py M6_BIG_SCALES), sixteen at most, in one of two alternating pattern buffers
 * ($6800 and $3000, 4096 words each): a pose change is written into the buffer that is not on screen, so no frame shows half of each. The entries are (variant * 8
 * + pose) * 4 + step, the palette is hardware palette 30 (the cache keeps off it). Admission is all or nothing, two units of every scanline a piece covers. */
static const uint16_t BIG_WORD[2]={0x6800,0x3000};
static uint16_t big_key[2]={0xffff,0xffff};static uint8_t big_shown,big_variant=255;
static bool big_draw(uint16_t key,int16_t sx,int16_t sy) {
    uint8_t e[10],n;uint32_t pieces,patterns;
    arcade_read(2,PCE_M6_BIG+(uint32_t)key*10,e,10);
    pieces=e[0]|(uint32_t)e[1]<<8|(uint32_t)e[2]<<16|(uint32_t)e[3]<<24;patterns=e[4]|(uint32_t)e[5]<<8|(uint32_t)e[6]<<16|(uint32_t)e[7]<<24;n=e[8];
    int16_t list[32];
    arcade_read(2,pieces,list,(uint16_t)n*4);
    uint8_t buf=big_key[big_shown]==key?big_shown:big_shown^1;
    if(big_key[buf]!=key) {
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
        arcade_vram(patterns,BIG_WORD[buf],(uint16_t)n*512);
        big_key[buf]=key;
    }
    big_shown=buf;
    uint8_t v=key>>5;
    if(big_variant!=v){uint8_t pal[32];arcade_read(2,PCE_M6_BIGPAL+(uint32_t)v*32,pal,32);vce_copy(30,pal,1);big_variant=v;}
    uint8_t first=sat_count,skipped=0;
    uint16_t pattern=BIG_WORD[buf]>>5;
    for(uint8_t i=0;i<n;++i) {
        int16_t x=sx+list[i*2],y=sy+list[i*2+1];
        if(x<=-32||x>=256||y<=-32||y>=224)continue;
        if(sat_count>=64)goto refuse;
        sprite_line_lo=y<0?0:y;sprite_line_hi=y+32>224?224:y+32;
        /* a piece that does not fit the scanlines (the arm takes them where it crosses the mech) is left out: the arm is in front of it there; more than four and the mech is refused */
        sprite_lines_reserve();if(!sprite_line_ok){if(++skipped>4)goto refuse;continue;}
        sprite_lines_reserve();if(!sprite_line_ok){sprite_lines_release();if(++skipped>4)goto refuse;continue;}
        sat[sat_page][sat_count++]=(vdc_sprite_t){y+64,x+32,pattern+i*8,VDC_SPRITE_FG|14|VDC_SPRITE_WIDTH_32|VDC_SPRITE_HEIGHT_32};
    }
    return true;
refuse:
    while(sat_count>first) {
        int16_t y=(int16_t)sat[sat_page][--sat_count].y-64;
        sprite_line_lo=y<0?0:y;sprite_line_hi=y+32>224?224:y+32;
        sprite_lines_release();sprite_lines_release();
    }
    return false;
}

/* The picture's call (m6_d.c blit): kind 0 the big mech (key, x, y), kind 1 the arm (frame, x, y, flip); a6.blit_ok is the answer. */
void m6_blit(void) {
    a6.blit_ok=a6.blit_kind?arm_draw((uint8_t)a6.blit_key,a6.blit_x,a6.blit_y,a6.blit_flip):big_draw(a6.blit_key,a6.blit_x,a6.blit_y);
}
/* The floor's raster tables (irq.S): for each of the 24 groups of four scanlines under the horizon, the BXR (the picture's scroll plus the strafing's shift of its depth band)
 * and the BYR (the texture line that depth asks for, 1926 / the group's row, plus the way walked: the texture's 96 lines repeat; the first five groups show the haze). */
void m6_floor(void) {
    extern uint8_t arena_floor[96];
    uint8_t *t=arena_floor;
    int16_t along=A.along>>7;
    for(uint8_t g=0;g<24;++g,t+=4) {
        uint8_t v0=m6_vtab[g];
        uint16_t line=224;
        if(v0!=255){int16_t v=v0+along;while(v>=96)v-=96;while(v<0)v+=96;line=128+v;}
        uint16_t byr=(line-1+A.floor_y)&0xff,bxr=(A.floor_x+(v0==255?0:(A.par[g<9?0:g<13?1:g<18?2:3]>>4)))&0x3ff;
        t[0]=bxr;t[1]=bxr>>8;t[2]=byr;t[3]=byr>>8;
    }
}
static void lock_update(void) {
    uint8_t best=255;int16_t ba=0x7fff;
    for(uint8_t k=0;k<3;++k) {
        Mech6 *m=&A.mech[k];if(m->st==S_OFF||m->st==S_DYING)continue;
        uint16_t d=m->dist>>2;int16_t a=abs16(relq(m->ang));
        if(d>=40&&d<=1600&&a<28*16&&a<ba){ba=a;best=k;}   /* under 0.13 radian of the aim */
    }
    if(best!=A.lock){A.lock=best;if(best!=255)audio_pcm_tick();}
}
static void shots_step(void) {
    for(uint8_t k=0;k<16;++k) {
        Shot6 *s=&A.shot[k];if(!s->life)continue;
        s->ang=wrapq(s->ang+s->vang);s->dist=(uint16_t)((int16_t)s->dist+s->vd);s->z+=s->vz;--s->life;
        if(!s->life||s->z<0){s->life=0;continue;}
        if(!s->enemy) {
            for(uint8_t j=0;j<3&&s->life;++j) {
                Mech6 *m=&A.mech[j];if(m->st==S_OFF||m->st==S_DYING)continue;
                int16_t da=s->ang-m->ang;if(da>ARC/2)da-=ARC;else if(da<-ARC/2)da+=ARC;
                if(s->dist+200>=m->dist&&abs16(da)<(int16_t)(rcp_of(m->dist)<<2)) {   /* within 64 units of its middle (the picture's zoom is 1.6) */
                    s->life=0;mech_damage(m,1,false);audio_effect(22);burst(m->ang,m->dist,s->z,0,9);   /* (a bolt strikes: the PSG zap, voice 1, no DDA) */
                }
            }
            for(uint8_t j=0;j<16&&s->life;++j) {   /* bolts shoot plasma down */
                Shot6 *o=&A.shot[j];if(!o->life||!o->enemy)continue;
                int16_t da=s->ang-o->ang;uint16_t dd=s->dist>o->dist?s->dist-o->dist:o->dist-s->dist;
                if(abs16(da)<160&&dd<240){s->life=0;o->life=0;burst(o->ang,o->dist,o->z,0,30);}
            }
        } else if(s->dist<=240) {   /* within 48 units of Ramrod: it hits unless Ramrod has stepped aside since it was fired */
            s->life=0;
            if(abs16(A.lat-s->lat0)<384){burst(s->ang,s->dist,s->z,2,9);hurt_player(s->dmg,16);}
        }
    }
}
/* Ramrod is down (m6_common.h hurt_player: campaign_hurt left pce_death): the arena stands still under the red flash; then, with a spare, the wave begins again at once
 * (the PC's begin_wave), with full armour and a moment's grace, nothing reloaded and no screen change; with none left the game is over. */
static bool dead(void) {
    if(A.shake)--A.shake;if(A.flash_red)--A.flash_red;
    if(!A.dead_t){A.shake=24;A.flash_red=40;A.msg=0;A.punch_t=-1;}
    if(++A.dead_t<110)return true;
    A.dead_t=0;pce_death=0;
    if(!pce_campaign.lives){pce_campaign.state=CAM_OVER;pce_campaign.timer=0;return true;}
    --pce_campaign.lives;
    for(uint8_t k=0;k<3;++k)A.mech[k].st=S_OFF;
    for(uint8_t k=0;k<16;++k)A.shot[k].life=0;
    for(uint8_t k=0;k<10;++k)A.fx[k].dur=0;
    A.spawned=A.killed=0;A.wave_t=0;A.heat=0;A.overheated=0;A.banner=150;A.lock=255;A.speed=A.strafe_v=A.turn_v=0;A.hurt=90;
    pce_metrics.hp=100;
    return false;
}
static void tick(void) {
    uint8_t w=pce_campaign.wave;
    if(pce_death&&dead())return;
    if(w==2&&!A.boss_music){A.boss_music=1;audio_music(8);}   /* the Commander's wave has the boss music (from the moment its dialogue is over) */
    ++A.wave_t;
    if(A.shake)--A.shake;if(A.flash_red)--A.flash_red;if(A.flash_white)--A.flash_white;if(A.banner)--A.banner;
    if(A.msg_t){if(!--A.msg_t)A.msg=0;}
    uint8_t active=0;
    for(uint8_t k=0;k<3;++k)if(A.mech[k].st!=S_OFF&&A.mech[k].st!=S_DYING)++active;
    while(A.spawned<WAVE_N[w]&&A.wave_t>=W_DELAY[w][A.spawned]+60&&active<WAVE_MAX[w]){A.sp_w=w;A.sp_i=A.spawned;overlay_call(M6B_BANK,m6_spawn);++A.spawned;++active;}
    if(A.spawned<WAVE_N[w]&&!active&&A.wave_t>60&&A.wave_t<W_DELAY[w][A.spawned]+60)A.wave_t=W_DELAY[w][A.spawned]+60;   /* nobody left: the next one comes now */
    overlay_call(M6A_BANK,m6_player);
    for(uint8_t k=0;k<3;++k)if(A.mech[k].st!=S_OFF){A.cur=k;overlay_call(M6B_BANK,m6_step);}
    shots_step();
    for(uint8_t k=0;k<10;++k)if(A.fx[k].dur&&++A.fx[k].t>=A.fx[k].dur)A.fx[k].dur=0;
    lock_update();
    A.left=WAVE_N[w]-A.killed;
    uint8_t alive=0;for(uint8_t k=0;k<3;++k)if(A.mech[k].st!=S_OFF)++alive;
    if(A.spawned>=WAVE_N[w]&&!alive) {
        for(uint8_t k=0;k<16;++k)if(A.shot[k].enemy)A.shot[k].life=0;
        if(w==2){pce_campaign.result=1;pce_campaign.story=3;pce_campaign.event=1;}
        else {
            pce_campaign.story=w+1;pce_campaign.event=1;++pce_campaign.wave;
            A.spawned=A.killed=0;A.wave_t=0;A.heat=0;A.overheated=0;A.banner=150;
            uint16_t hp=pce_metrics.hp+(pce_options.difficulty==2?25:40);   /* April patches the armour between waves; before the squadron she reroutes everything */
            if(w==1&&pce_options.difficulty<2)hp=100;
            pce_metrics.hp=hp>100?100:hp;
        }
    }
}
void m6_start(void) {
    uint8_t *p=(uint8_t*)&A;
    for(uint16_t k=0;k<sizeof A;++k)p[k]=0;
    A.rng=0x5AB3;A.lock=255;A.punch_t=-1;A.floor_x=385;A.floor_y=0;m6_floor();pce_arena_raster=1;
    pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;
    arcade_vram(PCE_M6_BOLTS,0x7c80,768);   /* the bolt's three frames: $7c80 middle, $7cc0 far, $7d00 very near (32x32) */
    {   /* palette 31, the HUD's (the cache would load it with the first banner; the fixed HUD does not go through the cache) */
        uint32_t entry;uint8_t pal[32];
        arcade_read(2,video_scene_ptr->sprites+(uint32_t)pce_hud_base[5]*16+8,&entry,4);arcade_read(2,entry,pal,32);vce_copy(31,pal,1);
    }
    uint8_t w=pce_control.phase<3?pce_control.phase:0;
    pce_campaign.wave=w;pce_campaign.boss_hp=0;pce_campaign.boss_kind=0;pce_metrics.hp=100;
    A.banner=150;A.left=WAVE_N[w];
    /* the boulders and cacti, spread round the outpost's field (ramrod.c place_props) */
    for(uint8_t k=0;k<14;++k) {
        uint8_t u=rnd();
        A.prop[k]=(Prop6){(int16_t)(((uint16_t)rnd()*84)),(uint16_t)(1200+(uint16_t)rnd()*20),u<90?0:u<165?1:2};
        if(A.prop[k].ang>=ARC)A.prop[k].ang-=ARC;
    }
}
void m6_frame(void) {
    for(uint8_t i=0;i<pce_control.elapsed&&!pce_campaign.event&&!pce_campaign.result&&pce_campaign.state==CAM_PLAY;++i)tick();
    overlay_call(M6D_BANK,m6_draw);
    if(pce_campaign.event) {
        arm_key[0]=arm_key[1]=255;   /* a dialogue is coming: it will write over the arm's second pattern buffer (after the draw: a punch still in flight has just loaded it) */
        /* the world holds still behind a dialogue: no shake left in the scroll or the floor (the panel's cells and its sprite corners are placed from the scroll of the last frame) */
        A.shake=0;pce_scroll_x=(uint16_t)((A.cam>>4)+385);pce_scroll_y=0;A.floor_x=pce_scroll_x;A.floor_y=0;m6_floor();
    }
}
