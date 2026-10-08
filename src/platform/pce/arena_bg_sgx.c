#pragma clang section text=".ram_bank113.text" rodata=".ram_bank113.rodata" data=".ram_bank113.data" bss=".ram_bank113.bss"
#include "arcade_pce.h"
#include "assets.h"
#include "m6/m6_state.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#include "video_pce.h"
#include <pce/hardware.h>
#include <pce/vdc.h>

#ifdef PCE_SGX

extern uint8_t buffer[2048];
extern volatile uint8_t pce_vdc_index;

static void arena_vdc_index(uint8_t reg) {
    pce_vdc_index=reg;
    *(volatile uint8_t *)0x20f7=reg;
    *IO_VDC_INDEX=reg;
}

static void arena_vdc_write(uint8_t reg,uint16_t value) {
    arena_vdc_index(reg);
    *IO_VDC_DATA_LO=(uint8_t)value;
    *IO_VDC_DATA_HI=(uint8_t)(value>>8);
}

/* VDC0 becomes the transparent BG0 object plane; its panorama/floor map is
 * streamed on VDC1. Tile zero is transparent in mode 1, so clear its BAT and
 * the 16-word character along with it before stage 6 begins. */
#define SGX_ARENA_CLEAR __attribute__((noinline,section(".ram_bank116.text")))
SGX_ARENA_CLEAR void video_arena_bg_clear_body(void) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    uint16_t control=*(volatile uint16_t *)0x20f3;
    uint16_t quiet=(control&~0x3000)&~VDC_CONTROL_ENABLE_BG;
    pce_vdc_index=VDC_REG_CONTROL;*(volatile uint8_t *)0x20f7=VDC_REG_CONTROL;
    *IO_VDC_INDEX=VDC_REG_CONTROL;*(volatile uint16_t *)0x20f3=quiet;
    *IO_VDC_DATA_LO=(uint8_t)quiet;*IO_VDC_DATA_HI=(uint8_t)(quiet>>8);
    pce_vdc_index=VDC_REG_VRAM_WRITE_ADDR;*(volatile uint8_t *)0x20f7=VDC_REG_VRAM_WRITE_ADDR;
    *IO_VDC_INDEX=VDC_REG_VRAM_WRITE_ADDR;*IO_VDC_DATA_LO=0;*IO_VDC_DATA_HI=0;
    pce_vdc_index=VDC_REG_VRAM_DATA;*(volatile uint8_t *)0x20f7=VDC_REG_VRAM_DATA;
    *IO_VDC_INDEX=VDC_REG_VRAM_DATA;
    for(uint16_t i=0;i<2064;++i){*IO_VDC_DATA_LO=0;*IO_VDC_DATA_HI=0;}
    (void)*IO_VDC_DATA_LO;
    pce_vdc_index=VDC_REG_CONTROL;*(volatile uint8_t *)0x20f7=VDC_REG_CONTROL;
    *IO_VDC_INDEX=VDC_REG_CONTROL;*(volatile uint16_t *)0x20f3=control;
    *IO_VDC_DATA_LO=(uint8_t)control;*IO_VDC_DATA_HI=(uint8_t)(control>>8);
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
}

static int16_t pixel_tile(int16_t p) {
    p+=p<0?-4:4;
    return p>>3;
}

static void arena_map_row(uint8_t page,uint8_t y,uint8_t first,uint8_t end,
                          const uint16_t *cells) {
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    arena_vdc_write(VDC_REG_VRAM_WRITE_ADDR,
                    (uint16_t)y*64+(uint16_t)page*32+first);
    arena_vdc_index(VDC_REG_VRAM_DATA);
    for(uint8_t x=first;x<end;++x) {
        uint16_t word=cells[x];
        *IO_VDC_DATA_LO=(uint8_t)word;
        *IO_VDC_DATA_HI=(uint8_t)(word>>8);
    }
    (void)*IO_VDC_DATA_LO;
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
}

/* The two 256-character pages alternate in the inactive BAT half. A pose is
 * converted offline to BG-format 8x8 tiles; the live draw only uploads a new
 * pose when a page's cache key changes and rewrites its dirty BAT rows. */
void pce_sgx_arena_bg_draw_body(void) {
    uint8_t active=(pce_sgx_metrics.paired_screen&PCE_SGX_ARENA_BG_PAGE)!=0;
    uint8_t page=active^1;
    uint16_t key=a6.blit_key,tile_count=0;
    uint32_t pattern_address=0,map_address=0;
    uint8_t *record=buffer+32,*list=buffer+1024;
    uint16_t *row=(uint16_t *)(buffer+1792);
    uint8_t min_x=32,max_x=0,min_y=32,max_y=0;
    uint8_t map_valid=0,bg_ok=1;
    int16_t anchor_x=pixel_tile(a6.blit_x),anchor_y=pixel_tile(a6.blit_y);

    if(key!=0xffff) {
        if(key>=96||!arcade_read(2,PCE_M6_BIG_BG+(uint32_t)key*12,record,12)) {
            bg_ok=0;
        } else {
            pattern_address=(uint32_t)record[0]|(uint32_t)record[1]<<8|
                (uint32_t)record[2]<<16|(uint32_t)record[3]<<24;
            map_address=(uint32_t)record[4]|(uint32_t)record[5]<<8|
                (uint32_t)record[6]<<16|(uint32_t)record[7]<<24;
            tile_count=record[8]|(uint16_t)record[9]<<8;
            if(!tile_count||tile_count>256||
               !arcade_read(2,map_address,list,tile_count*3)) {
                tile_count=0;bg_ok=0;
            }
        }
        if(bg_ok) {
            for(uint16_t i=0;i<tile_count;++i) {
                int16_t x=anchor_x+(int8_t)list[i*3];
                int16_t y=anchor_y+(int8_t)list[i*3+1];
                if(x<0||x>=32||y<0||y>=32)continue;
                if(!map_valid||x<min_x)min_x=x;
                if(!map_valid||x+1>max_x)max_x=x+1;
                if(!map_valid||y<min_y)min_y=y;
                if(!map_valid||y+1>max_y)max_y=y+1;
                map_valid=1;
            }
            if(!map_valid)bg_ok=0;
        }
        if(bg_ok&&(!a6.bg_cache_valid[page]||a6.bg_key[page]!=key)) {
            uint16_t bytes=tile_count<<5;
            uint16_t word=PCE_BG_WORD+(uint16_t)page*0x1000;
            if(!arcade_vram_to(0,pattern_address,word,bytes))bg_ok=0;
            else {
                a6.bg_key[page]=key;
                a6.bg_cache_valid[page]=1;
                pce_metrics.uploads+=bytes;
            }
        }
        if(bg_ok) {
            uint8_t variant=key>>5;
            if(!a6.bg_palette_valid||a6.bg_palette_variant!=variant) {
                if(!arcade_read(2,PCE_M6_BIGPAL+(uint32_t)variant*32,buffer,32))bg_ok=0;
                else {
                    vce_copy(15,buffer,1);
                    a6.bg_palette_variant=variant;
                    a6.bg_palette_valid=1;
                }
            }
        }
        if(!bg_ok) {tile_count=0;map_valid=0;}
    }

    uint8_t old_valid=a6.bg_map_valid[page];
    uint8_t y0=old_valid?a6.bg_min_y[page]:32;
    uint8_t y1=old_valid?a6.bg_max_y[page]:0;
    if(map_valid) {
        if(min_y<y0)y0=min_y;
        if(max_y>y1)y1=max_y;
    }
    uint8_t old_x0=old_valid?a6.bg_min_x[page]:32;
    uint8_t old_x1=old_valid?a6.bg_max_x[page]:0;
    uint16_t cursor=0;
    uint16_t chars=(uint16_t)PCE_BG_WORD/16+((uint16_t)page<<8);

    if(y0<y1) {
        uint16_t control=*(volatile uint16_t *)0x20f3;
        uint8_t index=pce_vdc_index;
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
        arena_vdc_write(VDC_REG_CONTROL,control&~0x3000);
        __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
        for(uint8_t y=y0;y<y1;++y) {
            while(cursor<tile_count&&anchor_y+(int8_t)list[cursor*3+1]<y)++cursor;
            for(uint8_t x=0;x<32;++x)row[x]=0;
            uint16_t i=cursor;
            while(i<tile_count) {
                int16_t ty=anchor_y+(int8_t)list[i*3+1];
                if(ty>y)break;
                int16_t x=anchor_x+(int8_t)list[i*3];
                if(ty==y&&x>=0&&x<32)
                    row[x]=(uint16_t)(0xf000|(chars+list[i*3+2]));
                ++i;
            }
            cursor=i;
            uint8_t x0=old_valid&&y>=a6.bg_min_y[page]&&y<a6.bg_max_y[page]?old_x0:32;
            uint8_t x1=x0<32?old_x1:0;
            if(map_valid&&y>=min_y&&y<max_y) {
                if(min_x<x0)x0=min_x;
                if(max_x>x1)x1=max_x;
            }
            if(x0<x1)arena_map_row(page,y,x0,x1,row);
        }
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
        arena_vdc_write(VDC_REG_CONTROL,control);
        arena_vdc_index(index);
        __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    }
    a6.bg_map_valid[page]=map_valid;
    if(map_valid) {
        a6.bg_min_x[page]=min_x;a6.bg_max_x[page]=max_x;
        a6.bg_min_y[page]=min_y;a6.bg_max_y[page]=max_y;
    }
    a6.bg_actor_drawn=(key!=0xffff&&bg_ok&&map_valid);
    pce_sgx_arena_bg_pending_page=page;
}

#endif
