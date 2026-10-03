#include <pce-cd.h>
#include "ipl_font.h"

/* pce-mkcd resolves these references against app.elf. The IPL executes in
 * console RAM, so loading and mapping $68-$6c cannot replace its code. */
extern char __cd_app_elf__sector[], __cd_app_elf__sector_count[];
extern char __cd_app_elf__sym___pce_app_entry[];

__attribute__((noinline)) static void open_memory(void) {
    __attribute__((leaf)) asm volatile("ldx #0\n1: lda $2000,x\npha\ninx\ncpx #32\nbne 1b\njsr $e0de\nldx #31\n2: pla\nsta $2000,x\ndex\nbpl 2b" ::: "a", "x", "y", "p", "memory");
}

__attribute__((noinline)) static void run_app(void) {
    uint32_t n = (uint32_t)__cd_app_elf__sector;
    pce_sector_t s = {.lo = n, .md = n >> 8, .hi = n >> 16};
    uint8_t result = pce_cdb_cd_read(s, PCE_CDB_BANK_MPR2, 0x68,
                                  (uint16_t)__cd_app_elf__sector_count);
    *(volatile uint8_t *)0x33ff = result;
    if (result) { for (;;) {} }
    __attribute__((leaf)) asm volatile(
        "lda #0x68\n tam #4\n lda #0x69\n tam #8\n"
        "lda #0x6a\n tam #16\n lda #0x6b\n tam #32\n"
        "lda #0x6c\n tam #64\n" ::: "a", "p");
    ((void (*)(void))__cd_app_elf__sym___pce_app_entry)();
    for (;;) {}
}

int main(void) {
    if((pce_cdb_version()>>8)<3) {
        pce_cpu_irq_disable();
        pce_vdc_poke(VDC_REG_MEMORY,VDC_BG_SIZE_64_32);
        pce_vdc_poke(VDC_REG_VRAM_WRITE_ADDR,0);
        *IO_VDC_INDEX=2;
        for(uint16_t k=0;k<2048;++k)*IO_VDC_DATA=0;
        pce_vdc_poke(VDC_REG_VRAM_WRITE_ADDR,0);
        *IO_VDC_INDEX=2;for(uint8_t k=0;k<16;++k)*IO_VDC_DATA=0;
        pce_vdc_poke(VDC_REG_VRAM_WRITE_ADDR,0x0800);*IO_VDC_INDEX=2;
        for(uint8_t glyph=0;glyph<sizeof(error_font)/8;++glyph)
            for(uint8_t planes=0;planes<2;++planes)for(uint8_t row=0;row<8;++row) {
                uint8_t bits=error_font[glyph*8+row];*IO_VDC_DATA=bits|(uint16_t)bits<<8;
            }
        pce_vdc_poke(VDC_REG_VRAM_WRITE_ADDR,10*64+3);*IO_VDC_INDEX=2;
        for(uint8_t glyph=0;glyph<sizeof(error_font)/8;++glyph)*IO_VDC_DATA=0xf080+glyph;
        pce_vce_set_color(0,0);pce_vce_set_color(255,0x1ff);
        *(volatile uint16_t*)0x20f3=0x0088;
        pce_vdc_poke(VDC_REG_CONTROL,0x88);pce_cpu_irq_enable();
        for(;;){}
    }
    open_memory();
    pce_cdb_cd_base((pce_sector_t){.lo=0}, PCE_CDB_BASE_SET_BOTH);
    run_app();
    for (;;) {}
}
