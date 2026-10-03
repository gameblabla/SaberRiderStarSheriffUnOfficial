/* Arcade Card block readers. Both data-port mirrors ($1a00/$1a01) auto-increment,
 * so TAI's alternating source reads the stream at 6 CPU cycles per byte. The
 * stub lives in resident RAM and is patched by arcade_read. No LLVM registers
 * are touched. IRQs may interrupt a block move; the BMT instruction is restartable. */
.section .text.irq,"ax",@progbits
.global arcade_tai, arcade_tai_src, arcade_tai_dst, arcade_tai_len
.global arcade_vdc_chunk, arcade_vdc_words
arcade_tai:
    .byte 0xf3
arcade_tai_src:
    .word 0x1a00
arcade_tai_dst:
    .word 0
arcade_tai_len:
    .word 0
    rts
/* Copy arcade_vdc_words words from Arcade port 0 to VDC VRAM data. The VDC
 * register index and write address are already selected. IRQs are masked, so
 * a VBlank/raster handler cannot disturb the VDC data latch between bytes. */
arcade_vdc_chunk:
    php
    sei
    ldx arcade_vdc_words
.Lword:
    lda $1a00
    sta $0002
    lda $1a00
    sta $0003
    dex
    bne .Lword
    plp
    rts
arcade_vdc_words:
    .byte 0
