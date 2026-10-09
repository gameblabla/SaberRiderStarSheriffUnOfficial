/* Arcade Card block readers. Both data-port mirrors ($1a00/$1a01) auto-increment,
 * so TAI's alternating source reads the stream at 6 CPU cycles per byte. The
 * stub lives in resident RAM and is patched by arcade_read. No LLVM registers
 * are touched. */
.section .text.irq,"ax",@progbits
.global arcade_tai, arcade_tai_src, arcade_tai_dst, arcade_tai_len
.comm arcade_tai_left,2
/* A block transfer cannot be interrupted, and a long one (the piece list of a sprite is up to 255 bytes, 1500 cycles) held back the
 * road's per-scanline raster interrupt by more than a scanline: BXR/BYR arrived a line late and one line of the road flashed out of
 * place. The caller sets the whole length; it is moved in bursts of at most 16 bytes (113 cycles) with interrupts open between them. */
arcade_tai:
    lda arcade_tai_len
    sta arcade_tai_left
    lda arcade_tai_len+1
    sta arcade_tai_left+1
.Lnext:
    lda arcade_tai_left+1
    bne .Lfull
    lda arcade_tai_left
    beq .Lend
    cmp #17
    bcc .Lset
.Lfull:
    lda #16
.Lset:
    sta arcade_tai_len
    stz arcade_tai_len+1
    sec
    lda arcade_tai_left
    sbc arcade_tai_len
    sta arcade_tai_left
    bcs .Lgo
    dec arcade_tai_left+1
.Lgo:
    .byte 0xf3
arcade_tai_src:
    .word 0x1a00
arcade_tai_dst:
    .word 0
arcade_tai_len:
    .word 0
    clc
    lda arcade_tai_dst
    adc arcade_tai_len
    sta arcade_tai_dst
    bcc .Lnext
    inc arcade_tai_dst+1
    bra .Lnext
.Lend:
    rts
/* Keep the stream loop in always-mapped resident RAM ($68), leaving work
 * bank $6b room for PCM's sample-bank crossing code. Interrupts may use MPR6
 * between bursts; the audio handler restores the caller's mapping. */
.section .text.irq,"ax",@progbits
.global arcade_vdc_copy, arcade_vdc_len
arcade_vdc_copy:
    tma #0x40
    pha
    lda #0x40
    tam #0x40
.Lburst:
#ifdef PCE_SGX
    /* A PCM tick spans 1024 CPU cycles. A 32-byte transfer masks IRQs
     * for 209 cycles; the race raster retains its shorter 16-byte bursts. */
    ldx pce_raster_enabled
    bne .Lshort
    lda #128
    ldx pce_pcm_active
    beq .Lsize
    lda #32
    bra .Lsize
.Lshort:
#endif
    lda #16
#ifdef PCE_SGX
.Lsize:
#endif
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
    .word 0xc000
.global arcade_vdc_dest
arcade_vdc_dest:
    .word 0x0002, 64
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
