#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pce-cd.h>

#define PCE_SCREEN_W 256
#define PCE_SCREEN_H 224
#define PCE_ARCADE_BYTES 0x200000UL
#define PCE_CODE __attribute__((noinline, section(".ram_bank105.text")))
#define PCE_FLOOR __attribute__((noinline, section(".ram_bank109.text")))
#define PCE_FLOW __attribute__((noinline, minsize, section(".ram_bank110.text")))
#define PCE_MISSION __attribute__((noinline, section(".ram_bank111.text")))
#define PCE_COMBAT __attribute__((noinline, minsize, section(".ram_bank112.text")))
#define PCE_TABLE __attribute__((section(".ram_bank106.rodata")))
#define PCE_BOSS __attribute__((noinline, section(".ram_bank123.text")))   /* $7b: spare code bank beside the CD buffer */
#define PCE_HUD __attribute__((noinline, section(".ram_bank124.text")))    /* $7c: spare code bank (HUDs of the race, cockpit, space) */
#define PCE_RACE __attribute__((noinline, section(".ram_bank121.text")))   /* $79 */
#define PCE_RACE2 __attribute__((noinline, minsize, section(".ram_bank122.text")))  /* $7a */
/* $76/$77: the CD transfer buffer, free between scene loads. loader_archive reads their code back from the disc after every load, so code
 * here must never call a loader (it would be overwritten while it runs). overlay_call bank 0x76 / 0x77. */
#define PCE_X1 __attribute__((noinline, section(".ram_bank118.text")))
#define PCE_X2 __attribute__((noinline, section(".ram_bank119.text")))
#define PCE_X3 __attribute__((noinline, section(".ram_bank117.text")))   /* $75: the audio bank (never overwritten) */
#define PCE_RENDER __attribute__((noinline, section(".ram_bank106.text")))
#define PCE_WORK __attribute__((section(".ram_bank107.work")))
#define PCE_STAGE __attribute__((section(".ram_bank108.stage")))
#ifdef PCE_SGX
#define PCE_ACTOR_DRAW_BANK 0x77
#else
#define PCE_ACTOR_DRAW_BANK 0x74
#endif
/* $68 resident, $69 platform/$6d floor overlays share MPR3, $6a renderer.
 * $6b work, $6c staging. BIOS remains in MPR7; MPR0 is hardware, MPR1 RAM.
 * Audio code is $75; CD scratch is $76-$7c. No stack, return address or IRQ lives there. */
#define PCE_BAT_WORD 0x0000
#define PCE_BG_WORD 0x0800
#define PCE_SPR_WORD 0x4800
#define PCE_SAT_WORD 0x7f00
#define PCE_SAT_ALT_WORD 0x7e00   /* $7800-$7dff remains available to the arena HUD/bolts */
#define PCE_BG_MAX_TILES 896
#define PCE_FONT_WORD 0x4200
#define PCE_SPR_BYTES 0x6c00

_Static_assert(sizeof(int) == 2 && sizeof(void *) == 2, "PCE ABI changed");
_Static_assert(sizeof(uint32_t) == 4, "32-bit asset handles required");

/* Palette RAM (the VCE) is only touched in vertical blank, or with the display off (the real chip shows a CPU access during the picture as
 * a stray dot; an emulator does not). Everything goes through these calls (vce_pce.c, bank $75), which map the SDK's names:
 *  - vce_copy / pce_vce_copy_palette of ONE palette while the display is on is queued, and the VBlank interrupt writes it (irq.S), at the same
 *    moment the new SAT comes up; it never blocks (only when four are already waiting);
 *  - more than one palette, a single colour or a read-back waits for the vertical blank first (a fade step, a flash);
 *  - vce_copy_now is for code that has just returned from video_wait and wants the write at once (a cell restore timed to the beam);
 *  - with the display off everything is immediate. */
extern volatile uint8_t pce_display_on;
/* A full-screen palette flash (scenery_pce.c space_screen_flash) holds the VCE white and keeps what the palettes should be in flash_palette: while vce_hold is set,
 * every write below goes into that copy instead of the chip, every read-back is answered from it, and the flash's end writes the copy out. (A snapshot taken at the
 * start and put back at the end lost everything written in between: a hull's or a sprite's colours, restored stale, or left white.) video_scene clears it. */
extern volatile uint8_t vce_hold;
extern uint16_t flash_palette[512];
void vce_copy(uint8_t index,const void *source,uint8_t count),vce_set(uint16_t index,uint16_t value),vce_read(void *dest,uint8_t index,uint8_t count);   /* video_pce.c ($6a, always mapped) */
void vce_copy_now(uint8_t index,const void *source,uint8_t count);
#ifndef VCE_RAW
#define pce_vce_copy_palette vce_copy
#define pce_vce_set_color vce_set
#define pce_vce_copy_palette_to_ram vce_read
#endif
