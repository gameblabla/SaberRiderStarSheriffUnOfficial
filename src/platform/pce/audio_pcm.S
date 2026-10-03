/* The BIOS timer vector jumps here: return with RTI, unlike VDC callbacks.
 * No compiler registers, zero-page pointers or C calls. The sample banks are
 * $7d-$7f; preserve MPR6 so interrupted Arcade transfers and code keep their map.
 * One original 5-bit PCM sample per 1024 high-speed CPU clocks. */
.section .text.irq,"ax",@progbits
.global pce_pcm_irq, pce_pcm_read
.extern pce_pcm_left, pce_pcm_bankid
pce_pcm_irq:
    pha
    stz 0x1403
    lda pce_pcm_left
    ora pce_pcm_left+1
    beq .Lpcm_end
    tma #0x40
    pha
    lda pce_pcm_bankid
    tam #0x40
    stz 0x0800
pce_pcm_read:
    lda 0xc000
    sta 0x0806
    inc pce_pcm_read+1
    bne .Lpcm_count
    inc pce_pcm_read+2
.Lpcm_count:
    lda pce_pcm_left
    bne .Lpcm_dec
    dec pce_pcm_left+1
.Lpcm_dec:
    dec pce_pcm_left
    pla
    tam #0x40
    pla
    rti
.Lpcm_end:
    stz 0x0c01
    stz 0x0800
    lda #16
    sta 0x0806
    stz 0x0804
    pla
    rti
