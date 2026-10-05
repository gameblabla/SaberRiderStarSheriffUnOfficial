/* One of Ramrod's four code images (src/platform/pce/m6/): its entry table at $6000, then everything of its object. */
SECTIONS {
  .m6 0x6000 : {
    KEEP(*(.m6entry))
    *(.text .text.* .rodata .rodata.* .data .data.* .bss .bss.* COMMON)
  }
  /DISCARD/ : { *(.comment) *(.llvm_addrsig) *(.llvm_sympart) *(.rela*) *(.note*) *(.zp*) }
}
ASSERT(SIZEOF(.m6) <= 0x2000, "an arena code image must fit its 8 KiB bank")
