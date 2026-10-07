/* CPU-only state belongs in low work RAM. Preserve high RAM for executable
 * code, stacks and buffers that the SCU can actually DMA. */
MEMORY { cpu_low (rw) : ORIGIN = 0x00200000, LENGTH = 0x000C8000 }
SECTIONS {
    .saturn_low_bss (NOLOAD) : {
        ___saturn_low_bss_start = .;
        *(.saturn_low_bss)
        . = ALIGN(64);
        ___saturn_low_bss_end = .;
    } > cpu_low
}
ASSERT(SIZEOF(.saturn_low_bss) <= 0x50000, "CPU-only state leaves too little low work RAM")
