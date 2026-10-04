/* Timer-delivered exact Build 14 DAC bytes, decoded during asset generation.
 * BIOS jumps here (RTI). No compiler scratch; preserve A and MPR6; X/Y are untouched.
 * Service lives in the always-mapped work bank; MPR3 is never changed. */
.section .text.irq,"ax",@progbits
.global pce_pcm_irq, pce_pcm_irq_end
.global pce_pcm_voices, pce_pcm_active
pce_pcm_voices = 0x2080
pce_pcm_active = 0x20a0
pce_pcm_irq:
    pha
    stz 0x1403
    tma #0x40
    pha
    jsr pce_pcm_service
    pla
    tam #0x40
    pla
    rti
pce_pcm_irq_end:

.section .ram_bank107.text,"ax",@progbits
.global pce_pcm_service, pce_pcm_service_end, pce_pcm_step
/* Unroll the two fixed voices: no indexed state accesses, X save/restore,
 * or per-voice calls. State and every DAC byte retain their original meaning. */
.macro voice base, bit, channel
    bbr\bit $20a0, .Ldone\@
    lda #\channel
    sta 0x0800
    lda \base+9
    tam #0x40
    lda (\base+2)
    sta 0x0806
    sta \base+4
    inc \base+2
    bne .Lcount\@
    inc \base+3
    lda \base+3
    cmp #0xe0
    bne .Lcount\@
    lda #0xc0
    sta \base+3
    inc \base+9
.Lcount\@:
    lda \base
    bne .Ldec\@
    dec \base+1
.Ldec\@:
    dec \base
    bne .Ldone\@
    lda \base+1
    bne .Ldone\@
    lda \base+10
    beq .Lmute\@
    lda \base+14
    sta \base
    lda \base+15
    sta \base+1
    lda \base+12
    sta \base+2
    lda \base+13
    sta \base+3
    lda \base+6
    sta \base+9
    bra .Ldone\@
.Lmute\@:
    rmb\bit $20a0
    lda #16
    sta 0x0806
    stz 0x0804
.Ldone\@:
.endm
pce_pcm_service:
    voice $2080, 0, 0
    voice $2090, 1, 1
    lda $20a0
    bne 1f
    stz 0x0c01
1:
    rts
pce_pcm_service_end:
/* Isolated native test entry exercises the very same service as the IRQ. */
.section .text.irq,"ax",@progbits
pce_pcm_step:
    tma #0x40
    pha
    jsr pce_pcm_service
    pla
    tam #0x40
    rts
