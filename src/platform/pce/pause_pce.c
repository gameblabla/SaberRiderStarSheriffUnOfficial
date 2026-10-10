#include "video_pce.h"
#include "overlay_pce.h"
#include "sgx_pce.h"
#ifdef RETAIL
#ifdef PCE_SGX
#define PAUSE_CODE __attribute__((noinline,minsize,section(".ram_bank130.text")))
#define PAUSE_DATA __attribute__((section(".ram_bank130.rodata")))
#else
#define PAUSE_CODE PCE_FLOW
#define PAUSE_DATA __attribute__((section(".ram_bank110.rodata")))
#endif
/* Borrow three patterns at the end of the font while gameplay is frozen.
 * Preserve the actual VRAM SAT: foreground reordering means the RAM SAT is
 * not necessarily in display order. The first half of buffer remains intact
 * (it can hold a palette snapshot). No cache or scene state is changed. */
#define PAUSE_WORD (PCE_FONT_WORD+0x540)
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
static const uint8_t letters[5][7] PAUSE_DATA={
    {30,17,17,30,16,16,16}, /* P */
    {14,17,17,31,17,17,17}, /* A */
    {17,17,17,17,17,17,14}, /* U */
    {15,16,16,14,1,1,30},  /* S */
    {31,16,16,30,16,16,31} /* E */
};
PAUSE_CODE static void pause_copy(uint16_t word,uint16_t *data,uint16_t count,bool read) {
    for(uint16_t i=0;i<count;++i) {
        video_vdc(read?1:0,word+i);
        pce_cpu_irq_disable();
        pce_vdc_index=2;*(volatile uint8_t*)0x20f7=2;*IO_VDC_INDEX=2;
        if(read) {
            uint8_t lo=*IO_VDC_DATA_LO;
            data[i]=lo|((uint16_t)*IO_VDC_DATA_HI<<8);
        } else {
            *IO_VDC_DATA_LO=data[i];*IO_VDC_DATA_HI=data[i]>>8;
        }
        pce_cpu_irq_enable();
    }
}
PAUSE_CODE void pause_show_body(void) {
    uint16_t *saved=(uint16_t*)(buffer+1024);
    uint16_t target=video_sat_target();
    pause_copy(pce_sat_word,saved,256,true);
    pause_copy(PAUSE_WORD,saved+256,192,true);
    pce_vce_copy_palette_to_ram(saved+448,31,1);
    /* Palette 31 belongs to the HUD. Use its brightest/darkest opaque
     * entries for the label without recoloring the retained gameplay SAT. */
    uint8_t ink=1,outline=1,bright=0,dark=255;
    for(uint8_t i=1;i<16;++i) {
        uint16_t c=saved[448+i];uint8_t l=(c&7)+((c>>3)&7)+((c>>6)&7);
        if(l>bright){bright=l;ink=i;}if(l<dark){dark=l;outline=i;}
    }
    for(uint8_t cell=0;cell<3;++cell) {
        uint16_t *pat=(uint16_t*)sat[1];
        for(uint8_t i=0;i<64;++i)pat[i]=0;
        for(uint8_t k=0;k<5;++k)for(uint8_t row=0;row<7;++row)
            for(uint8_t col=0;col<5;++col)if(letters[k][row]&(16>>col)) {
                int8_t x=4+k*8+col-cell*16;
                if(x>=0&&x<16)pat[1+row]|=(uint16_t)1<<(15-x);
                for(int8_t dx=-1;dx<=1;++dx)if(x+dx>=0&&x+dx<16) {
                    uint16_t mask=(uint16_t)1<<(15-x-dx);
                    for(uint8_t y=row;y<=row+2;++y)pat[16+y]|=mask;
                }
            }
        for(uint8_t y=0;y<16;++y) {
            uint16_t fill=pat[y],edge=pat[16+y]&~fill;
            for(uint8_t plane=0;plane<4;++plane)
                pat[plane*16+y]=((ink&(1<<plane))?fill:0)|((outline&(1<<plane))?edge:0);
        }
        pause_copy(PAUSE_WORD+(uint16_t)cell*64,pat,64,false);
    }
    /* Put PAUSE first so the hardware sprite limits cannot hide the label.
     * The last three actors are omitted only while paused; all 64 restore. */
    pause_copy(target+12,saved,244,false);
#ifdef PCE_SGX
    /* The added herd row shares VDC0 with the retained foreground. Hiding
     * VDC1 alone only removes the original row. Keep the saved SAT intact
     * for unpause, but hide the VDC0 horses in the temporary pause table. */
    if(pce_sgx_gameplay()&&pce_metrics.stage==1)for(uint8_t i=0;i<61;++i)
        if((saved[(uint16_t)i*4+3]&15)==13) {
            video_vdc(0,target+12+(uint16_t)i*4);video_vdc(2,0);
        }
#endif
    vdc_sprite_t *label=sat[1];
    for(uint8_t i=0;i<3;++i)label[i]=(vdc_sprite_t){64+104,32+(pce_raster_enabled?232:104)+i*16,
        (PAUSE_WORD>>5)+i*2,VDC_SPRITE_FG|15};
    pause_copy(target,(uint16_t*)label,12,false);
    video_sat_replace(target);
}
PAUSE_CODE void pause_hide_body(void) {
    uint16_t *saved=(uint16_t*)(buffer+1024);
    pause_copy(PAUSE_WORD,saved+256,192,false);
    uint16_t target=video_sat_target();
    pause_copy(target,saved,256,false);
    video_sat_replace(target);
}
PCE_FLOW void pause_show(void) {
#ifdef PCE_SGX
    overlay_call(0x82,pause_show_body);
#else
    pause_show_body();
#endif
}
PCE_FLOW void pause_hide(void) {
#ifdef PCE_SGX
    overlay_call(0x82,pause_hide_body);
#else
    pause_hide_body();
#endif
}
#endif
