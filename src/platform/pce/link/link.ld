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
__ram_bank120 = 0x6000;
__ram_bank121 = 0x6000;
__ram_bank122 = 0x6000;
__ram_bank123 = 0x6000;
__ram_bank124 = 0x6000;
INCLUDE binary-scd.ld
__pce_app_entry = ABSOLUTE(_start);
ASSERT(__zp_bss_start + __zp_bss_size <= 0xf82080, "Audio state overlaps compiler zero page")
