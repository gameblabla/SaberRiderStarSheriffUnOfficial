/* Timer-delivered exact Build 14 DAC bytes, decoded during asset generation.
 * BIOS jumps here (RTI). No compiler scratch; preserve A/X and MPR6.
 * Service uses bank $75; preserve MPR3 without changing raster scheduling. */
.section .text.irq,"ax",@progbits
.global pce_pcm_irq, pce_pcm_irq_end
.global pce_pcm_voices, pce_pcm_active
pce_pcm_voices = 0x2080
pce_pcm_active = 0x20a0
pce_pcm_irq:
    pha
    phx
    stz 0x1403
    tma #0x40
    pha
    tma #0x08
    pha
    lda #0x75
    tam #0x08
    jsr pce_pcm_service
    pla
    tam #0x08
    pla
    tam #0x40
    plx
    pla
    rti
pce_pcm_irq_end:

.section .ram_bank117.text,"ax",@progbits
.global pce_pcm_service, pce_pcm_service_end, pce_pcm_step, pce_pcm_decode
/* State retains its 16-byte stride: left=0,read=2,last DAC=4,
 * initial bank=6,bank=9,loop=10,channel=11,start=12,count=14. */
pce_pcm_service:
    bbr0 $20a0, .Lsecond
    stz 0x0800
    ldx #0
    jsr pce_pcm_decode
.Lsecond:
    bbr1 $20a0, .Lidle_check
    lda #1
    sta 0x0800
    ldx #16
    jsr pce_pcm_decode
.Lidle_check:
    lda $20a0
    bne .Lservice_done
    stz 0x0c01
.Lservice_done:
    rts
/* Caller guarantees an active voice. X selects its 16-byte state.
 * Runtime shares the IRQ's single mapping save/restore across both voices. */
pce_pcm_decode:
    lda $2089,x
    tam #0x40
    lda ($2082,x)
    sta 0x0806
    sta $2084,x
    inc $2082,x
    bne .Lcount
    inc $2083,x
    /* $dfff -> $c000 in the next physical bank. */
    lda $2083,x
    cmp #0xe0
    bne .Lcount
    lda #0xc0
    sta $2083,x
    inc $2089,x
.Lcount:
    lda $2080,x
    bne .Ldec
    dec $2081,x
.Ldec:
    dec $2080,x
    bne .Lstep_done
    lda $2081,x
    bne .Lstep_done
    lda $208a,x
    beq .Lmute
    lda $208e,x
    sta $2080,x
    lda $208f,x
    sta $2081,x
    lda $208c,x
    sta $2082,x
    lda $208d,x
    sta $2083,x
    lda $2086,x
    sta $2089,x
    rts
.Lmute:
    cpx #0
    bne .Lmute_second
    rmb0 $20a0
    bra .Lmuted
.Lmute_second:
    rmb1 $20a0
.Lmuted:
    lda #16
    sta 0x0806
    stz 0x0804
.Lstep_done:
    rts
pce_pcm_service_end:
/* Native verification entry preserves the interrupted mapping too. */
pce_pcm_step:
    tma #0x40
    pha
    lda $208b,x
    sta 0x0800
    jsr pce_pcm_decode
    pla
    tam #0x40
    rts
