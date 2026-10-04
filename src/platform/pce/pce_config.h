#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pce-cd.h>

#define PCE_SCREEN_W 256
#define PCE_SCREEN_H 224
#define PCE_ARCADE_BYTES 0x200000UL
#define PCE_CODE __attribute__((noinline, section(".ram_bank105.text")))
#define PCE_FLOOR __attribute__((noinline, section(".ram_bank109.text")))
#define PCE_FLOW __attribute__((noinline, section(".ram_bank110.text")))
#define PCE_MISSION __attribute__((noinline, section(".ram_bank111.text")))
#define PCE_COMBAT __attribute__((noinline, section(".ram_bank112.text")))
#define PCE_TABLE __attribute__((section(".ram_bank106.rodata")))
#define PCE_RENDER __attribute__((noinline, section(".ram_bank106.text")))
#define PCE_WORK __attribute__((section(".ram_bank107.work")))
#define PCE_STAGE __attribute__((section(".ram_bank108.stage")))
/* $68 resident, $69 platform/$6d floor overlays share MPR3, $6a renderer.
 * $6b work, $6c staging. BIOS remains in MPR7; MPR0 is hardware, MPR1 RAM.
 * Audio code is $75; CD scratch is $76-$7c. No stack, return address or IRQ lives there. */
#define PCE_BAT_WORD 0x0000
#define PCE_BG_WORD 0x0800
#define PCE_SPR_WORD 0x4800
#define PCE_SAT_WORD 0x7f00
#define PCE_BG_MAX_TILES 928
#define PCE_FONT_WORD 0x4200
#define PCE_SPR_BYTES 0x6e00

_Static_assert(sizeof(int) == 2 && sizeof(void *) == 2, "PCE ABI changed");
_Static_assert(sizeof(uint32_t) == 4, "32-bit asset handles required");
