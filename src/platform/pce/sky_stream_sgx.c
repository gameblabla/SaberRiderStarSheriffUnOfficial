#include "sgx_pce.h"
#ifdef PCE_SGX
#include "arcade_pce.h"
#include "video_pce.h"
#include "assets.h"
#include "overlay_pce.h"
#include "sprite_cache_pce.h"
#include <pce/hardware.h>
#include <pce/vdc.h>

extern PceSgxSkyRecord pce_sgx_sky;
extern uint16_t pce_sgx_sky_first;
extern volatile uint16_t pce_sgx_sky_scroll_x,pce_scroll_x;
extern volatile uint8_t pce_display_on,pce_sat_pending;
extern uint8_t buffer[2048];
extern void pce_sgx_herd_sky_load_body(void);
extern uint8_t sat1_previous_count[2];
static uint16_t sky_cursor PCE_WORK;
static uint8_t sky_held_count PCE_WORK;
static uint16_t sky_near_first PCE_WORK;
static uint32_t sky_phase PCE_WORK;
static uint8_t sky_tick PCE_WORK;
volatile uint16_t pce_sgx_sky_near_x PCE_WORK;
volatile uint8_t pce_sgx_sky_split_line PCE_WORK;
#define SKY_DIR 0x0000
#define SKY_REFS 0x8000
#define SKY_IDS 0x8700
#define SKY_COLUMNS 0x8e00
#define SKY_HELD (SKY_COLUMNS+3960)
#define SKY_HOLD 0x8000
#define SKY_CODE __attribute__((noinline,minsize,section(".ram_bank128.text")))

/* Port 3's auto increment advances the base, just as the main BG directory
   does. Its directory uses bank $1e; this cache owns bank $1f. */
static SKY_CODE void cache_at(uint16_t address) {
    volatile uint8_t *r=(volatile uint8_t *)0x1a30;
    r[2]=address;r[3]=address>>8;r[4]=31;
}
static SKY_CODE uint16_t cache_get(uint16_t address) {
    cache_at(address);
    volatile uint8_t *r=(volatile uint8_t *)0x1a30;
    uint8_t low=r[0];return low|(uint16_t)r[0]<<8;
}
static SKY_CODE void cache_set(uint16_t address,uint16_t value) {
    cache_at(address);
    *(volatile uint8_t *)0x1a30=value;
    *(volatile uint8_t *)0x1a30=value>>8;
}
static SKY_CODE void cache_fill(uint16_t address,uint8_t value,uint16_t size) {
    cache_at(address);
    while(size--)*(volatile uint8_t *)0x1a30=value;
}
static SKY_CODE void cache_setup(void) {
    volatile uint8_t *r=(volatile uint8_t *)0x1a30;
    r[5]=0;r[6]=0;r[7]=1;r[8]=0;r[9]=0x11;
}

static inline void sky_index(uint8_t reg) {
    PCE_SGX_RECORD_VDC2_INDEX(reg);
    *IO_VDC2_INDEX=reg;
}
static inline void sky_write(uint8_t reg,uint16_t word) {
    sky_index(reg);
    *IO_VDC2_DATA_LO=word;*IO_VDC2_DATA_HI=word>>8;
}

SKY_CODE void pce_sgx_sky_load_body(void) {
    const PceScene *scene=&pce_scenes[pce_metrics.stage-1];
    pce_sgx_sky_first=sky_near_first=0xffff;sky_cursor=0;sky_held_count=0;
    sky_phase=0;sky_tick=pce_ticks;pce_sgx_sky_split_line=0;
    pce_sgx_metrics.paired_screen|=PCE_SGX_STATIC_SKY;
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    sky_write(VDC_REG_CONTROL,0);
    sky_write(VDC_REG_MEMORY,VDC_BG_SIZE_64_32);
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    if(!scene->occlusion||!arcade_read(2,scene->occlusion,&pce_sgx_sky,sizeof pce_sgx_sky)||
       !pce_sgx_sky.cols||!pce_sgx_sky.tiles||!pce_sgx_sky.map||
       !pce_sgx_sky.bytes||pce_sgx_sky.bytes>16384||
       (scene->horse&&pce_sgx_sky.bytes>16384)) {
        ++pce_sgx_metrics.failures;pce_sgx_metrics.paired_screen=0;return;
    }
    cache_setup();
    cache_fill(SKY_DIR,0xff,32768);
    cache_fill(SKY_REFS,0,1792);
    cache_fill(SKY_IDS,0xff,1792+3960);
    pce_sgx_sky_split_line=pce_sgx_sky.split_row*8;
    sprite_cache_foreground_first=pce_sgx_sky.foreground_first;
    sprite_cache_stage=pce_metrics.stage;
    if(scene->horse)pce_sgx_herd_sky_load_body();
    if(pce_sgx_sky.moon_patterns) {
        uint8_t colors[32];
        arcade_read(2,pce_sgx_sky.moon_palette,colors,32);pce_vce_copy_palette(29,colors,1);
        arcade_vram_to(1,pce_sgx_sky.moon_patterns,0x2800,4608);
    }
}

/* Separate row bands share one pattern directory, with independent source
 * windows and BAT rows. The horse scene caps the cache at $2800, where its
 * three resident poses begin; streamed panoramas can never overwrite them. */
SKY_CODE static uint16_t camera_offset(uint16_t speed) {
    return (pce_scroll_x>>8)*speed+(((pce_scroll_x&255)*speed+255)>>8);
}
SKY_CODE void pce_sgx_sky_stream_body(void) {
    const PceSgxSkyRecord *s=&pce_sgx_sky;
    if(!s->cols)return;
    uint8_t elapsed=pce_ticks-sky_tick;sky_tick=pce_ticks;
    uint16_t offset;
    if(s->wrap==1) {
        sky_phase+=(uint16_t)elapsed*s->speed;
        uint32_t period=(uint32_t)s->cols<<11;
        while(sky_phase>=period)sky_phase-=period;
        offset=sky_phase>>8;
    } else offset=camera_offset(s->speed);
    uint16_t offsets[2]={offset,camera_offset(s->near_speed)};
    uint16_t firsts[2]={offsets[0]>>3,offsets[1]>>3};
    uint16_t olds[2]={pce_sgx_sky_first,sky_near_first};
    uint8_t bands=s->split_row?2:1;
    bool jump=false;
    for(uint8_t b=0;b<bands;++b)
        if(olds[b]!=0xffff&&firsts[b]!=olds[b]&&firsts[b]!=olds[b]+1&&firsts[b]+1!=olds[b])jump=true;
    cache_setup();
    if(jump) {
        cache_fill(SKY_REFS,0,1792);cache_fill(SKY_COLUMNS,0xff,3960);
        olds[0]=olds[1]=0xffff;sky_held_count=0;
    }
    for(uint8_t k=0;k<sky_held_count;++k) {
        uint16_t slot=cache_get(SKY_HELD+(uint16_t)k*2);
        uint16_t a=SKY_REFS+slot*2,refs=cache_get(a);
        if(refs&SKY_HOLD)cache_set(a,refs&~SKY_HOLD);
    }
    sky_held_count=0;
    uint16_t limit=(pce_scenes[pce_metrics.stage-1].horse||s->moon_patterns)?512:PCE_BG_MAX_TILES;
    uint8_t *cells=buffer+1920;
    uint16_t control=pce_display_on?VDC_CONTROL_ENABLE_BG:0;
    if(pce_display_on&&!pce_sgx_vdc1_hidden)control|=VDC_CONTROL_ENABLE_SPRITE;
    for(uint8_t b=0;b<bands;++b) {
        uint16_t first=firsts[b],old=olds[b],from=first,end=first+33;
        if(first==old)continue;
        if(old!=0xffff) {
            if(first==old+1)from=old+33;
            else if(first+1==old)end=old;
        }
        uint8_t y0=b?s->split_row:0,y1=b?30:(s->split_row?s->split_row:30);
        uint16_t cols=b?s->near_cols:s->cols;
        uint32_t map=b?s->near_map:s->map;
        for(uint16_t col=from;col<end;++col) {
            uint16_t refs_address=SKY_COLUMNS+(b*33+col%33)*60;
            for(uint8_t y=y0;y<y1;++y) {
                uint16_t slot=cache_get(refs_address+y*2);
                if(slot==0xffff)continue;
                uint16_t a=SKY_REFS+slot*2,refs=cache_get(a)-1;
                cache_set(a,refs?refs:SKY_HOLD);
                if(!refs)cache_set(SKY_HELD+(uint16_t)sky_held_count++*2,slot);
            }
            uint16_t source=col;
            if((!b&&s->wrap)||b)source%=cols;
            if(source<cols) {
                if(!arcade_read(1,map+(uint32_t)source*90,cells,90)) {
                    ++pce_sgx_metrics.failures;return;
                }
            } else for(uint8_t i=0;i<90;++i)cells[i]=0;
            for(uint8_t y=y0;y<y1;++y) {
                uint16_t id=cells[y*3]|(uint16_t)cells[y*3+1]<<8;
                if(b)id+=s->near_first_tile;
                uint16_t slot=cache_get(SKY_DIR+id*2);
                if(slot==0xffff) {
                    uint16_t checked=0;slot=sky_cursor;
                    while(cache_get(SKY_REFS+slot*2)) {
                        if(++checked==limit){++pce_sgx_metrics.failures;return;}
                        if(++slot==limit)slot=0;
                    }
                    sky_cursor=slot+1;if(sky_cursor==limit)sky_cursor=0;
                    uint16_t previous=cache_get(SKY_IDS+slot*2);
                    if(previous!=0xffff)cache_set(SKY_DIR+previous*2,0xffff);
                    if(!arcade_vram_to(1,s->tiles+(uint32_t)id*32,PCE_BG_WORD+slot*16,32)) {
                        ++pce_sgx_metrics.failures;return;
                    }
                    cache_set(SKY_IDS+slot*2,id);cache_set(SKY_DIR+id*2,slot);
                }
                uint16_t a=SKY_REFS+slot*2;
                cache_set(a,(cache_get(a)&~SKY_HOLD)+1);cache_set(refs_address+y*2,slot);
                uint16_t word=(PCE_BG_WORD>>4)+slot+((uint16_t)cells[y*3+2]<<12);
                cells[y*3]=word;cells[y*3+1]=word>>8;
            }
            __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
            sky_write(VDC_REG_CONTROL,control|0x1000);
            sky_write(VDC_REG_VRAM_WRITE_ADDR,(uint16_t)y0*64+(col&63));
            sky_index(VDC_REG_VRAM_DATA);
            for(uint8_t y=y0;y<y1;++y) {
                *IO_VDC2_DATA_LO=cells[y*3];*IO_VDC2_DATA_HI=cells[y*3+1];
            }
            (void)*IO_VDC2_DATA_LO;sky_write(VDC_REG_CONTROL,control);
            __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
        }
    }
    pce_sgx_sky_first=firsts[0];sky_near_first=firsts[1];
    pce_sgx_sky_scroll_x=offsets[0];pce_sgx_sky_near_x=offsets[1];
}

extern vdc_sprite_t sat[2][64];
extern uint8_t sat_count,sprite_line_lo,sprite_line_hi,sprite_line_ok;
extern void sprite_lines_reserve(void),sprite_lines_release(void);
SKY_CODE void pce_sgx_moon_draw_body(void) {
    if(!pce_sgx_sky.moon_patterns)return;
    int16_t left=179-((uint32_t)pce_scroll_x*5>>8);
    for(uint8_t row=0;row<3;++row)for(uint8_t col=0;col<3;++col) {
        int16_t x=left+col*32,y=22+row*32;
        if(x<=-32||x>=256||sat_count==64)continue;
        sprite_line_lo=y;sprite_line_hi=y+32;
        sprite_lines_reserve();if(!sprite_line_ok)continue;
        sprite_lines_reserve();if(!sprite_line_ok){sprite_lines_release();continue;}
        sat[1][sat_count++]=(vdc_sprite_t){y+64,x+32,(0x2800>>5)+(row*3+col)*8,
            VDC_SPRITE_FG|13|VDC_SPRITE_WIDTH_32|VDC_SPRITE_HEIGHT_32};
    }
}

/* Replace only the old hull's SAT entries before changing its patterns. */
SKY_CODE void pce_sgx_hull_retire_body(void) {
    while(pce_sat_pending){}
    uint16_t source=pce_sgx_sat1_alt?PCE_SAT_ALT_WORD:PCE_SAT_WORD;
    uint16_t target=pce_sgx_sat1_alt?PCE_SAT_WORD:PCE_SAT_ALT_WORD;
    for(uint8_t k=0;k<64;++k) {
        uint16_t e[4];
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
        sky_write(VDC_REG_VRAM_READ_ADDR,source+(uint16_t)k*4);
        sky_index(VDC_REG_VRAM_DATA);
        for(uint8_t j=0;j<4;++j) {
            uint8_t lo=*IO_VDC2_DATA_LO;e[j]=lo|(uint16_t)*IO_VDC2_DATA_HI<<8;
        }
        uint16_t pattern=(e[2]&0x3ff)<<5;
        if((e[3]&15)==14||(pattern>=PCE_SPR_WORD+16*256&&pattern<PCE_SPR_WORD+40*256))e[0]=0;
        sky_write(VDC_REG_VRAM_WRITE_ADDR,target+(uint16_t)k*4);sky_index(VDC_REG_VRAM_DATA);
        for(uint8_t j=0;j<4;++j){*IO_VDC2_DATA_LO=e[j];*IO_VDC2_DATA_HI=e[j]>>8;}
        __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    }
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    sky_write(VDC_REG_SATB_START,target);pce_sgx_sat1_alt^=1;
    sat1_previous_count[0]=sat1_previous_count[1]=64;
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    video_wait();
}
#endif
