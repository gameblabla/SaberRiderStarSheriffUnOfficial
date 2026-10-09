#include "sgx_pce.h"
#ifdef PCE_SGX
#include "assets.h"
#include "arcade_pce.h"
#include "pce_config.h"
#include <pce/vdc.h>
#define ANIMATE __attribute__((noinline,minsize,section(".ram_bank135.text")))
volatile uint8_t pce_sgx_select_animation_phase PCE_WORK;
uint8_t pce_sgx_select_animation_reset PCE_WORK;
static uint8_t preparing PCE_WORK, pending PCE_WORK;
static uint16_t uploaded PCE_WORK;
static struct __attribute__((packed)) { uint32_t tiles,map; uint16_t count; } next PCE_WORK;

/* Both patterns and BAT stay invisible while uploading. Called just after
 * video_wait: expose a completed page in VBlank before beginning its successor.
 * Palettes are resident and shared, so rotation never changes portrait colors. */
ANIMATE void pce_sgx_select_animate_body(void) {
    if(PCE_SGX_SELECT_FRAMES<2)return;
    if(pce_sgx_select_animation_reset) {
        pce_sgx_select_animation_reset=0;pce_sgx_select_animation_phase=0;
        preparing=1;pending=0;uploaded=0;
    }
    if(pending) {
        __attribute__((leaf)) asm volatile("php\nsei" ::: "p", "memory");
        PCE_SGX_RECORD_VDC2_INDEX(VDC_REG_BG_SCROLL_Y);
        *(volatile uint8_t*)0x10=VDC_REG_BG_SCROLL_Y;
        *(volatile uint8_t*)0x12=0;
        *(volatile uint8_t*)0x13=preparing&1;
        __attribute__((leaf)) asm volatile("plp" ::: "p", "memory");
        pce_sgx_select_animation_phase=preparing;
        if(++preparing==PCE_SGX_SELECT_FRAMES)preparing=0;
        pending=0;uploaded=0;
    }
    if(!uploaded && !arcade_read(2,PCE_SGX_SELECT_ANIMATION+(uint32_t)preparing*10,&next,10))goto failed;
    uint16_t size=next.count*32;
    uint16_t bytes=size-uploaded;if(bytes>2048)bytes=2048;
    if(!arcade_vram_to(1,next.tiles+uploaded,(preparing&1?0x4000:0x1000)+(uploaded>>1),bytes))goto failed;
    uploaded+=bytes;
    if(uploaded<size)return;
    for(uint8_t row=0;row<28;++row)
        if(!arcade_vram_to(1,next.map+(uint16_t)row*80,(preparing&1?0x800:0)+(uint16_t)row*64,80))goto failed;
    pending=1;return;
failed:
    ++pce_sgx_metrics.failures;
}
#endif
