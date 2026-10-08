/* Pass this with Clang's -T, not -Wl,-T, to suppress its default IPL script. */
/* Stage code overlays share the $6000 execution window. */
__ram_bank109 = 0x6000;
__ram_bank110 = 0x6000;
__ram_bank111 = 0x6000;
__ram_bank112 = 0x6000;
__ram_bank113 = 0x6000;
__ram_bank114 = 0x6000;
__ram_bank115 = 0x6000;
__ram_bank116 = 0x6000;
__ram_bank117 = 0x6000;
__ram_bank118 = 0x6000;   /* $76-$77: the CD transfer buffer while a scene loads; code between loads (loader_pce.c code_restore) */
__ram_bank119 = 0x6000;
__ram_bank120 = 0x6000;
__ram_bank121 = 0x6000;
__ram_bank122 = 0x6000;
__ram_bank123 = 0x6000;
__ram_bank124 = 0x6000;
/* Physical bank $80 is the SGX sky overlay. Arena A links virtually at $90;
 * arena B-D remain virtual $81-$83. m6_image.py removes all arena images. */
__ram_bank128 = 0x6000;
__ram_bank144 = 0x6000;
__ram_bank129 = 0x6000;
__ram_bank130 = 0x6000;
__ram_bank131 = 0x6000;
/* Virtual arena storage is never loaded by the IPL. Its runtime image still
   executes from bank $76; this frees physical $80 for the sky streamer. */
__ram_bank144_bank = 144;
MEMORY { ram_bank144 (rwxa) : ORIGIN = 0x01906000, LENGTH = 0x2000 }
SECTIONS { .ram_bank144 : { *(.ram_bank144 .ram_bank144.*) } >ram_bank144 }
INCLUDE binary-scd.ld
__pce_app_entry = ABSOLUTE(_start);
ASSERT(__zp_bss_start + __zp_bss_size <= 0xf82080, "Audio state overlaps compiler zero page")

/* Shared pursuit RNG must be callable while any stage overlay is mapped. */
ASSERT((race_random & 0xffff) < 0x6000 || (race_random & 0xffff) >= 0x8000,
       "Race RNG must remain outside the overlay execution window")
ASSERT((sprite_lines_reserve & 0xffff) >= 0x8000 &&
       (sprite_lines_reserve & 0xffff) < 0xa000 &&
       (sprite_lines_release & 0xffff) >= 0x8000 &&
       (sprite_lines_release & 0xffff) < 0xa000,
       "Shared sprite admission must remain in the resident renderer bank")
ASSERT((hud_copy & 0xffff) >= 0x8000 && (hud_copy & 0xffff) < 0xa000 &&
       (hud_copy_src & 0xffff) >= 0x8000 && (hud_copy_src & 0xffff) < 0xa000 &&
       (hud_copy_dst & 0xffff) >= 0x8000 && (hud_copy_dst & 0xffff) < 0xa000,
       "Shared copy loop and patched operands must remain always mapped")
ASSERT((hud_copy_len & 0xffff) >= 0x8000 && (hud_copy_len & 0xffff) < 0xa000 &&
       (hud_copy_opcode & 0xffff) >= 0x8000 && (hud_copy_opcode & 0xffff) < 0xa000,
       "Shared copy length and opcode must remain always mapped")
