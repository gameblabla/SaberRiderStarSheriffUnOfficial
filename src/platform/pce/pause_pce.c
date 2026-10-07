#include "video_pce.h"
#ifdef RETAIL
/* Borrow three patterns at the end of the font while gameplay is frozen.
 * Preserve the actual VRAM SAT: foreground reordering means the RAM SAT is
 * not necessarily in display order. The first half of buffer remains intact
 * (it can hold a palette snapshot). No cache or scene state is changed. */
#define PAUSE_WORD (PCE_FONT_WORD+0x540)
extern uint8_t buffer[2048];
extern vdc_sprite_t sat[2][64];
static const uint8_t letters[5][7] __attribute__((section(".ram_bank110.rodata")))={
    {30,17,17,30,16,16,16}, /* P */
    {14,17,17,31,17,17,17}, /* A */
    {17,17,17,17,17,17,14}, /* U */
    {15,16,16,14,1,1,30},  /* S */
    {31,16,16,30,16,16,31} /* E */
};
PCE_FLOW static void pause_copy(uint16_t word,uint16_t *data,uint16_t count,bool read) {
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
PCE_FLOW void pause_show(void) {
    uint16_t *saved=(uint16_t*)(buffer+1024);
    uint16_t target=video_sat_target();
    pause_copy(pce_sat_word,saved,256,true);
    pause_copy(PAUSE_WORD,saved+256,192,true);
    pce_vce_copy_palette_to_ram(saved+448,31,1);
    /* Black outline and white ink, with transparent index zero. */
    /* sat_page remains zero; its unused second page is temporary storage. */
    uint16_t *colors=(uint16_t*)sat[1];
    for(uint8_t i=0;i<16;++i)colors[i]=saved[448+i];
    colors[14]=0;colors[15]=0x1ff;
    pce_vce_copy_palette(31,colors,1);
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
        for(uint8_t y=0;y<16;++y)pat[32+y]=pat[48+y]=pat[16+y];
        pause_copy(PAUSE_WORD+(uint16_t)cell*64,pat,64,false);
    }
    /* Put PAUSE first so the hardware sprite limits cannot hide the label.
     * The last three actors are omitted only while paused; all 64 restore. */
    pause_copy(target+12,saved,244,false);
    vdc_sprite_t *label=sat[1];
    for(uint8_t i=0;i<3;++i)label[i]=(vdc_sprite_t){64+104,32+(pce_raster_enabled?232:104)+i*16,
        (PAUSE_WORD>>5)+i*2,VDC_SPRITE_FG|15};
    pause_copy(target,(uint16_t*)label,12,false);
    video_sat_replace(target);
}
PCE_FLOW void pause_hide(void) {
    uint16_t *saved=(uint16_t*)(buffer+1024);
    pause_copy(PAUSE_WORD,saved+256,192,false);
    pce_vce_copy_palette(31,saved+448,1);
    uint16_t target=video_sat_target();
    pause_copy(target,saved,256,false);
    video_sat_replace(target);
}
#endif
