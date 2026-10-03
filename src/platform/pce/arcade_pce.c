#include "arcade_pce.h"

/* Do not use the SDK's page macros: port blocks have a $10 stride and the
 * offset is 16 bits. Port 0 uploads, 1 maps, 2 metadata, 3 cache directory. */
static volatile uint8_t *port_regs(uint8_t port) {
    return (volatile uint8_t *)(0x1a00 + ((uint16_t)port << 4));
}
bool arcade_detect(void) {
    return *(volatile uint8_t *)0x1aff == 0x51;
}
bool arcade_seek(uint8_t port, uint32_t address) {
    if (port > 3 || address >= PCE_ARCADE_BYTES) return false;
    volatile uint8_t *r = port_regs(port);
    r[2] = address; r[3] = address >> 8; r[4] = address >> 16;
    r[5] = 0; r[6] = 0; r[7] = 1; r[8] = 0; r[9] = 0x11;
    return true;
}
static bool range(uint32_t a, uint16_t n) {
    return a < PCE_ARCADE_BYTES && n <= PCE_ARCADE_BYTES - a;
}
bool arcade_read(uint8_t port, uint32_t address, void *out, uint16_t size) {
    if (!range(address, size) || !arcade_seek(port, address)) return false;
    uint8_t *d = out;
    volatile uint8_t *r = port_regs(port);
    while (size--) *d++ = *r;
    return true;
}
bool arcade_write(uint8_t port, uint32_t address, const void *in, uint16_t size) {
    if (!range(address, size) || !arcade_seek(port, address)) return false;
    const uint8_t *s = in;
    volatile uint8_t *r = port_regs(port);
    while (size--) *r = *s++;
    return true;
}
bool arcade_fill(uint32_t address,uint8_t value,uint16_t size) {
    if(!range(address,size)||!arcade_seek(3,address))return false;
    while(size--) *(volatile uint8_t*)0x1a30=value;
    return true;
}
bool arcade_vram(uint32_t address, uint16_t word, uint16_t size) {
    if ((size & 1) || !range(address, size) ||
        word > 0x8000U - (size >> 1) || !arcade_seek(0, address)) return false;
    __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
    pce_vdc_poke(VDC_REG_VRAM_WRITE_ADDR, word);
    *(volatile uint8_t*)0x20f7=VDC_REG_VRAM_DATA;
    *IO_VDC_INDEX = VDC_REG_VRAM_DATA;
    __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
    /* Small pairs remain interruptible between words. The raster handler
     * restores the VDC index; no long block transfer masks raster IRQs. */
    while (size) {
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p","memory");
        *IO_VDC_DATA_LO = *(volatile uint8_t *)0x1a00;
        *IO_VDC_DATA_HI = *(volatile uint8_t *)0x1a00;
        __attribute__((leaf)) asm volatile("plp" ::: "p","memory");
        size -= 2;
    }
    return true;
}
uint8_t arcade_selftest(void) {
    uint8_t mask = 0, data[8], got[8];
    for (uint8_t p = 0; p < 4; ++p) {
        for (uint8_t i = 0; i < 8; ++i) data[i] = 0x39 + p * 17 + i;
        /* Cross a 64 KiB address carry, with separate locations per port. */
        uint32_t a = 0x1afffcUL + ((uint32_t)p << 16);
        arcade_write(p, a, data, 8);
        arcade_read(p, a, got, 8);
        uint8_t i;
        for (i = 0; i < 8 && got[i] == data[i]; ++i) {}
        if (i == 8) mask |= 1 << p;
    }
    return mask;
}
