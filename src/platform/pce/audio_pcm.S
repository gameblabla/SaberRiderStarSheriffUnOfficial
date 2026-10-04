/* Timer-delivered Build 14 software ADPCM, two independent PSG DDA voices.
 * BIOS jumps to this hook (RTI). No LLVM registers or compiler scratch.
 * Preserve MPR3 here; packed-byte fetches locally preserve MPR6.
 * The raster driver keeps its own IRQ scheduling, including the race renderer. */
.section .text.irq,"ax",@progbits
.global pce_pcm_irq
/* Dedicated direct-page state, disjoint from LLVM $00-$1f, renderer
 * scratch $c0-$df, and BIOS $e0-$ff. C sees full CPU addresses. */
.global pce_pcm_voices, pce_pcm_active
pce_pcm_voices = 0x2080
pce_pcm_active = 0x20a0
pce_pcm_irq:
    pha
    phx
    phy
    stz 0x1403
    tma #0x08
    pha
    lda #0x75
    tam #0x08
    cld
    jsr pce_pcm_service
    pla
    tam #0x08
    ply
    plx
    pla
    rti
.global pce_pcm_irq_end
pce_pcm_irq_end:

.section .ram_bank117.text,"ax",@progbits
.global pce_pcm_step, pce_pcm_service
.extern pce_adpcm_small_lo,pce_adpcm_small_hi,pce_adpcm_large_lo,pce_adpcm_large_hi
.extern pce_adpcm_next_small,pce_adpcm_next_large
/* State offsets: left=0,read=2,predictor=4,index=6,packed=7,phase=8,
 * bank=9,loop=10,channel=11,start=12,count=14. X is 0/16.
 * phase counts down 4..0; the shifted code bits also supply ADC/SBC carry. */
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
/* Caller guarantees an active voice. X selects its 16-byte state. */
pce_pcm_step:
    /* Standalone entry for native codec verification. Runtime selects the
     * fixed PSG channel in the dispatcher and skips these two loads. */
    ldy $208b,x
    sty 0x0800
.global pce_pcm_decode
pce_pcm_decode:
    lda $2088,x
    bne .Lpair
    /* Mapping and fetching happen once per four output samples. The state
     * pointer is directly usable by HuC6280's indexed-indirect mode. */
    tma #0x40
    pha
    lda $2089,x
    tam #0x40
    lda ($2082,x)
    sta $2087,x
    pla
    tam #0x40
    lda #4
    sta $2088,x
    inc $2082,x
    bne .Lpair
    inc $2083,x
.Lpair:
    ldy $2086,x
    lda $2087,x
    lsr
    bcs .Llarge_sign
    lsr
    bcs .Lsmall_sub
.Lsmall_add:
    lda pce_adpcm_next_small,y
    sta $2086,x
    lda $2084,x
    adc pce_adpcm_small_lo,y
    sta $2084,x
    lda $2085,x
    adc pce_adpcm_small_hi,y
    bcc .Lstorehi
    lda #255
    sta $2084,x
    bra .Lstorehi
.Llarge_sign:
    lsr
    bcs .Llarge_sub
.Llarge_add:
    lda pce_adpcm_next_large,y
    sta $2086,x
    lda $2084,x
    adc pce_adpcm_large_lo,y
    sta $2084,x
    lda $2085,x
    adc pce_adpcm_large_hi,y
    bcc .Lstorehi
    lda #255
    sta $2084,x
    bra .Lstorehi
.Lsmall_sub:
    lda pce_adpcm_next_small,y
    sta $2086,x
    lda $2084,x
    sbc pce_adpcm_small_lo,y
    sta $2084,x
    lda $2085,x
    sbc pce_adpcm_small_hi,y
    bcs .Lstorehi
    lda #0
    sta $2084,x
    bra .Lstorehi
.Llarge_sub:
    lda pce_adpcm_next_large,y
    sta $2086,x
    lda $2084,x
    sbc pce_adpcm_large_lo,y
    sta $2084,x
    lda $2085,x
    sbc pce_adpcm_large_hi,y
    bcs .Lstorehi
    lda #0
    sta $2084,x
    bra .Lstorehi
.Lstorehi:
    sta $2085,x
    /* One DDA channel per voice, rather than the demo's weighted 8-bit pair. */
    lsr
    lsr
    lsr
    sta 0x0806
    lsr $2087,x
    lsr $2087,x
    dec $2088,x
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
    /* Loop from the same initial decoder state, including partial final bytes. */
    lda $208e,x
    sta $2080,x
    lda $208f,x
    sta $2081,x
    lda $208c,x
    sta $2082,x
    lda $208d,x
    sta $2083,x
    stz $2084,x
    lda #128
    sta $2085,x
    stz $2086,x
    stz $2088,x
    rts
.Lmute:
    ldy $208b,x
    lda .Lchannel_mask,y
    and $20a0
    sta $20a0
    lda #16
    sta 0x0806
    stz 0x0804
.Lstep_done:
    rts

.Lchannel_mask:
    .byte 254,253
