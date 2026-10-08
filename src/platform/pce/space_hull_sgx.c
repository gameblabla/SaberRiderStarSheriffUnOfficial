#pragma clang section text=".ram_bank135.text" rodata=".ram_bank135.rodata" data=".ram_bank135.data" bss=".ram_bank135.bss"
#include "arcade_pce.h"
#include "assets.h"
#include "overlay_pce.h"
#include "scenery_pce.h"
#include "sgx_pce.h"
#include "video_pce.h"
#include <pce/bank.h>
#include <pce/hardware.h>
#include <pce/vdc.h>

#ifdef PCE_SGX
#define HULL_COLS 28
#define HULL_ROWS 16
#define HULL_MAP_BYTES (HULL_COLS * HULL_ROWS * 2)
#define HULL_TILES_OFFSET (192 + ((HULL_MAP_BYTES + 63) & ~63))

extern uint8_t buffer[2048];
extern uint16_t hull_palette[64];
extern uint8_t space_hull_port_x[7],space_hull_port_y[7];
extern uint16_t space_hull_char;
extern uint8_t space_hull_draw_flash,space_hull_draw_gone,hull_lit,hull_gone;
extern int16_t space_hull_draw_y;
extern volatile uint8_t pce_vdc_index;
volatile uint8_t space_hull_sgx_ok PCE_WORK;

static __attribute__((always_inline)) void hull_index(uint8_t reg) {
    pce_vdc_index=reg;
    *(volatile uint8_t *)0x20f7=reg;
    *IO_VDC_INDEX=reg;
}

static __attribute__((always_inline)) void hull_write(uint8_t reg,uint16_t value) {
    hull_index(reg);
    *IO_VDC_DATA_LO=(uint8_t)value;
    *IO_VDC_DATA_HI=(uint8_t)(value>>8);
}

static __attribute__((noinline,section(".ram_bank113.text"))) void hull_clear_bat(void) {
    uint8_t saved=pce_vdc_index;
    uint16_t blank=PCE_BG_WORD>>4;
    for(uint16_t start=0;start<2048;start+=16) {
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
        hull_write(VDC_REG_VRAM_WRITE_ADDR,start);
        hull_index(VDC_REG_VRAM_DATA);
        for(uint8_t i=0;i<16;++i) {
            *IO_VDC_DATA_LO=(uint8_t)blank;
            *IO_VDC_DATA_HI=(uint8_t)(blank>>8);
        }
        __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    }
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    hull_index(saved);
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
}

__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_load_body(void) {
    uint32_t record=pce_sgx_boss_bg[6];
    uint16_t count=0;
    space_hull_sgx_ok=0;
    if(!record||!arcade_read(2,record,&count,2)||count<6||count>512||
       !arcade_read(2,record+4,space_hull_top,HULL_COLS)||
       !arcade_read(2,record+4+HULL_COLS,space_hull_bottom,HULL_COLS)||
       !arcade_read(2,record+64,hull_palette,128)) {
        ++pce_sgx_metrics.failures;
        return;
    }

    uint32_t patterns=record+HULL_TILES_OFFSET;
    uint16_t bytes=(uint16_t)(count*32);
    if(!arcade_vram_to(0,patterns,PCE_BG_WORD,bytes)||
       !arcade_read(2,patterns+bytes+(uint32_t)(count-5)*32,buffer,32)) {
        ++pce_sgx_metrics.failures;
        return;
    }
    space_hull_char=(PCE_BG_WORD>>4)+count-5;
    pce_vce_copy_palette(10,buffer,1);
    pce_vce_copy_palette(11,hull_palette,4);
    pce_vce_set_color(255,0x1ff);
    const uint8_t px[7]={112,132,154,174,206,213,199};
    const uint8_t py[7]={50,50,49,48,75,65,84};
    for(uint8_t k=0;k<7;++k){space_hull_port_x[k]=px[k];space_hull_port_y[k]=py[k];}
    space_hull_sgx_ok=1;
}

__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_bat_body(void) {
    if(!space_hull_sgx_ok)return;
    uint16_t *cells=(uint16_t *)(buffer+1024);
    uint32_t record=pce_sgx_boss_bg[6];
    if(!arcade_read(2,record+192,cells,HULL_MAP_BYTES)) {
        ++pce_sgx_metrics.failures;
        space_hull_sgx_ok=0;
        return;
    }
    uint8_t saved=pce_vdc_index;
    hull_clear_bat();
    for(uint8_t y=0;y<HULL_ROWS;++y) {
        for(uint8_t first=0;first<HULL_COLS;first+=16) {
            __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
            hull_write(VDC_REG_VRAM_WRITE_ADDR,(uint16_t)y*64+first);
            hull_index(VDC_REG_VRAM_DATA);
            uint8_t end=first+16>HULL_COLS?HULL_COLS:first+16;
            for(uint8_t x=first;x<end;++x) {
                uint16_t source=cells[(uint16_t)y*HULL_COLS+x];
                uint16_t cell=PCE_BG_WORD>>4;
                if(source) {
                    uint8_t palette=(source>>12)&3;
                    cell=(source&0x0fff)+(PCE_BG_WORD>>4);
                    cell|=(uint16_t)(11+palette)<<12;
                }
                *IO_VDC_DATA_LO=(uint8_t)cell;
                *IO_VDC_DATA_HI=(uint8_t)(cell>>8);
            }
            __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
        }
    }
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    (void)*IO_VDC_DATA_LO;
    hull_index(saved);
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
}

__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_draw_body(void) {
    if(!space_hull_ready)return;
    video_scroll((uint16_t)(-(pce_sgx_gameplay()?16:76)-space_hull_x),(uint16_t)-space_hull_draw_y);
    if(space_hull_draw_gone&&!hull_gone) {
        hull_clear_bat();
        hull_gone=1;
    }
    if(hull_lit==(space_hull_draw_flash!=0))return;
    hull_lit=space_hull_draw_flash!=0;
    for(uint8_t i=0;i<64;++i) {
        uint16_t color=hull_palette[i];
        if(space_hull_draw_flash&&(i&15))color=0x1ff;
        ((uint16_t*)buffer)[i]=color;
    }
    pce_vce_copy_palette(pce_sgx_gameplay()?11:0,buffer,4);
}

/* These entry points keep the hull work in bank $71, leaving MPR6 mapped to
   its stage-data bank for palette and scratch-buffer accesses. */
__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_load_wrapper(void) {
    space_hull_sgx_load_body();
}

__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_bat_wrapper(void) {
    space_hull_sgx_bat_body();
}

__attribute__((noinline,section(".ram_bank113.text"))) void space_hull_sgx_draw_wrapper(void) {
    space_hull_sgx_draw_body();
}
#endif
