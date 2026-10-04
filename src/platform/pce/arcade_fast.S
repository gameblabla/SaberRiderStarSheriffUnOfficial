/* Arcade Card block readers. Both data-port mirrors ($1a00/$1a01) auto-increment,
 * so TAI's alternating source reads the stream at 6 CPU cycles per byte. The
 * stub lives in resident RAM and is patched by arcade_read. No LLVM registers
 * are touched. IRQs may interrupt a block move; the BMT instruction is restartable. */
.section .text.irq,"ax",@progbits
.global arcade_tai, arcade_tai_src, arcade_tai_dst, arcade_tai_len
arcade_tai:
    .byte 0xf3
arcade_tai_src:
    .word 0x1a00
arcade_tai_dst:
    .word 0
arcade_tai_len:
    .word 0
    rts
/* The always-mapped work bank has room for the stream loop. Interrupts may
 * use MPR6 between bursts; the audio handler restores the caller's mapping. */
.section .ram_bank107.text,"ax",@progbits
.global arcade_vdc_copy, arcade_vdc_len
arcade_vdc_copy:
    tma #0x40
    pha
    lda #0x40
    tam #0x40
.Lburst:
    lda #64
    ldx arcade_vdc_len+1
    bne 1f
    cmp arcade_vdc_len
    bcc 1f
    lda arcade_vdc_len
1:
    sta .Ltransfer+5
    php
    sei
.Ltransfer:
    .byte 0xe3
    .word 0xc000, 0x0002, 64
    plp
    sec
    lda arcade_vdc_len
    sbc .Ltransfer+5
    sta arcade_vdc_len
    bcs 1f
    dec arcade_vdc_len+1
1:
    ora arcade_vdc_len+1
    bne .Lburst
    pla
    tam #0x40
    rts
arcade_vdc_len:
    .word 0
