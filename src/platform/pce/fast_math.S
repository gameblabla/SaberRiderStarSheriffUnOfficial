/* Exact compiler-ABI arithmetic for HuC6280. No SDK numeric object is
 * linked: byte products use quarter squares; division normalizes the divisor
 * and tests only quotient bits that can be nonzero. Imaginary registers
 * $02-$0f are call-clobbered. IRQs do not use them and remain enabled.
 * The immutable table is loaded with overlay $80 but read through MPR6;
 * restore that mapping before returning to any caller (including MPR6 code).
 */
#ifdef PCE_SGX
.section .ram_bank128.rodata.math,"a",@progbits
.balign 256
.global pce_qsquare_low
pce_qsquare_low:
    .byte 0,0,1,2,4,6,9,12,16,20,25,30,36,42,49,56
    .byte 64,72,81,90,100,110,121,132,144,156,169,182,196,210,225,240
    .byte 0,16,33,50,68,86,105,124,144,164,185,206,228,250,17,40
    .byte 64,88,113,138,164,190,217,244,16,44,73,102,132,162,193,224
    .byte 0,32,65,98,132,166,201,236,16,52,89,126,164,202,241,24
    .byte 64,104,145,186,228,14,57,100,144,188,233,22,68,114,161,208
    .byte 0,48,97,146,196,246,41,92,144,196,249,46,100,154,209,8
    .byte 64,120,177,234,36,94,153,212,16,76,137,198,4,66,129,192
    .byte 0,64,129,194,4,70,137,204,16,84,153,222,36,106,177,248
    .byte 64,136,209,26,100,174,249,68,144,220,41,118,196,18,97,176
    .byte 0,80,161,242,68,150,233,60,144,228,57,142,228,58,145,232
    .byte 64,152,241,74,164,254,89,180,16,108,201,38,132,226,65,160
    .byte 0,96,193,34,132,230,73,172,16,116,217,62,164,10,113,216
    .byte 64,168,17,122,228,78,185,36,144,252,105,214,68,178,33,144
    .byte 0,112,225,82,196,54,169,28,144,4,121,238,100,218,81,200
    .byte 64,184,49,170,36,158,25,148,16,140,9,134,4,130,1,128
    .byte 0,128,1,130,4,134,9,140,16,148,25,158,36,170,49,184
    .byte 64,200,81,218,100,238,121,4,144,28,169,54,196,82,225,112
    .byte 0,144,33,178,68,214,105,252,144,36,185,78,228,122,17,168
    .byte 64,216,113,10,164,62,217,116,16,172,73,230,132,34,193,96
    .byte 0,160,65,226,132,38,201,108,16,180,89,254,164,74,241,152
    .byte 64,232,145,58,228,142,57,228,144,60,233,150,68,242,161,80
    .byte 0,176,97,18,196,118,41,220,144,68,249,174,100,26,209,136
    .byte 64,248,177,106,36,222,153,84,16,204,137,70,4,194,129,64
    .byte 0,192,129,66,4,198,137,76,16,212,153,94,36,234,177,120
    .byte 64,8,209,154,100,46,249,196,144,92,41,246,196,146,97,48
    .byte 0,208,161,114,68,22,233,188,144,100,57,14,228,186,145,104
    .byte 64,24,241,202,164,126,89,52,16,236,201,166,132,98,65,32
    .byte 0,224,193,162,132,102,73,44,16,244,217,190,164,138,113,88
    .byte 64,40,17,250,228,206,185,164,144,124,105,86,68,50,33,16
    .byte 0,240,225,210,196,182,169,156,144,132,121,110,100,90,81,72
    .byte 64,56,49,42,36,30,25,20,16,12,9,6,4,2,1,0
pce_qsquare_high:
    .byte 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    .byte 1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2
    .byte 2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3
    .byte 4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,6
    .byte 6,6,6,6,6,7,7,7,7,7,7,8,8,8,8,8
    .byte 9,9,9,9,9,9,10,10,10,10,10,11,11,11,11,12
    .byte 12,12,12,12,13,13,13,13,14,14,14,14,15,15,15,15
    .byte 16,16,16,16,17,17,17,17,18,18,18,18,19,19,19,19
    .byte 20,20,20,21,21,21,21,22,22,22,23,23,23,24,24,24
    .byte 25,25,25,25,26,26,26,27,27,27,28,28,28,29,29,29
    .byte 30,30,30,31,31,31,32,32,33,33,33,34,34,34,35,35
    .byte 36,36,36,37,37,37,38,38,39,39,39,40,40,41,41,41
    .byte 42,42,43,43,43,44,44,45,45,45,46,46,47,47,48,48
    .byte 49,49,49,50,50,51,51,52,52,53,53,53,54,54,55,55
    .byte 56,56,57,57,58,58,59,59,60,60,61,61,62,62,63,63
    .byte 64,64,65,65,66,66,67,67,68,68,69,69,70,70,71,71
    .byte 72,72,73,73,74,74,75,76,76,77,77,78,78,79,79,80
    .byte 81,81,82,82,83,83,84,84,85,86,86,87,87,88,89,89
    .byte 90,90,91,92,92,93,93,94,95,95,96,96,97,98,98,99
    .byte 100,100,101,101,102,103,103,104,105,105,106,106,107,108,108,109
    .byte 110,110,111,112,112,113,114,114,115,116,116,117,118,118,119,120
    .byte 121,121,122,123,123,124,125,125,126,127,127,128,129,130,130,131
    .byte 132,132,133,134,135,135,136,137,138,138,139,140,141,141,142,143
    .byte 144,144,145,146,147,147,148,149,150,150,151,152,153,153,154,155
    .byte 156,157,157,158,159,160,160,161,162,163,164,164,165,166,167,168
    .byte 169,169,170,171,172,173,173,174,175,176,177,178,178,179,180,181
    .byte 182,183,183,184,185,186,187,188,189,189,190,191,192,193,194,195
    .byte 196,196,197,198,199,200,201,202,203,203,204,205,206,207,208,209
    .byte 210,211,212,212,213,214,215,216,217,218,219,220,221,222,223,224
    .byte 225,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239
    .byte 240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255

.section .text.pce_fast_math,"ax",@progbits
/* Full unsigned byte product: A * X -> A:X, clobbers $08,$09,$0c,$0d.
 * Page alignment makes each half directly indexable: carry selects the
 * second page of q(a+b), while |a-b| always fits the first page. */
.Lproduct8:
    sta $2008
    stx $2009
    tma #64
    pha
    lda #128
    tam #64
    lda $2008
    clc
    adc $2009
    tay
    bcc 2f
    lda pce_qsquare_cpu+256,y
    sta $200c
    lda pce_qsquare_cpu+768,y
    bra 3f
2:
    lda pce_qsquare_cpu,y
    sta $200c
    lda pce_qsquare_cpu+512,y
3:
    sta $200d
    lda $2008
    sec
    sbc $2009
    bcs 1f
    eor #255
    inc
1:
    tay
    lda $200c
    sec
    sbc pce_qsquare_cpu,y
    sta $200c
    lda $200d
    sbc pce_qsquare_cpu+512,y
    tax
    pla
    tam #64
    lda $200c
    rts

.global __wrap___mulqi3, __wrap___mulhi3, __wrap___mulsi3
.global pce_fast_mul8, pce_fast_mul16, pce_fast_mul32
__wrap___mulqi3:
pce_fast_mul8:
    jmp .Lproduct8
/* Three byte products suffice for the low word. */
__wrap___mulhi3:
pce_fast_mul16:
    sta $2004
    stx $2005
    lda $2002
    sta $2006
    lda $2003
    sta $2007
    lda $2004
    ldx $2006
    jsr .Lproduct8
    ldy $2005
    bne 1f
    ldy $2007
    bne 1f
    rts
1:
    pha
    phx
    lda $2005
    beq 2f
    lda $2005
    ldx $2006
    jsr .Lproduct8
    bra 3f
2:
    cla
3:
    sta $200e
    lda $2007
    beq 4f
    lda $2004
    ldx $2007
    jsr .Lproduct8
    bra 5f
4:
    cla
5:
    clc
    adc $200e
    sta $200e
    pla
    clc
    adc $200e
    tax
    pla
    rts
/* Long multiply: stop at the highest set multiplier bit. Shifts and adds
 * use RAM throughout, avoiding the SDK's register shuffles each iteration.
 * Products retain their low 32 bits, including negative operands. */
__wrap___mulsi3:
pce_fast_mul32:
    /* Archive offsets overwhelmingly multiply a word by a byte. Compute
       the exact 24-bit result with two table reads, not a long shift loop. */
    tay
    lda $2002
    ora $2003
    ora $2005
    ora $2006
    ora $2007
    bne .Lmul32_wide
    stx $2006
    lda $2004
    sta $2007
    tax
    tya
    jsr .Lproduct8
    pha
    phx
    lda $2006
    ldx $2007
    jsr .Lproduct8
    sta $2005
    stx $2002
    pla
    clc
    adc $2005
    tax
    lda $2002
    adc #0
    sta $2002
    stz $2003
    pla
    rts
.Lmul32_wide:
    tya
    sta $2008
    stx $2009
    lda $2002
    sta $200a
    lda $2003
    sta $200b
    stz $200c
    stz $200d
    stz $200e
    stz $200f
1:
    lsr $2007
    ror $2006
    ror $2005
    ror $2004
    bcc 2f
    clc
    lda $200c
    adc $2008
    sta $200c
    lda $200d
    adc $2009
    sta $200d
    lda $200e
    adc $200a
    sta $200e
    lda $200f
    adc $200b
    sta $200f
2:
    lda $2004
    ora $2005
    ora $2006
    ora $2007
    beq 3f
    asl $2008
    rol $2009
    rol $200a
    rol $200b
    bra 1b
3:
    lda $200e
    sta $2002
    lda $200f
    sta $2003
    lda $200c
    ldx $200d
    rts

/* Byte quotient and remainder share normalization/subtraction. */
.global __wrap___udivqi3, __wrap___umodqi3, pce_fast_udiv8, pce_fast_umod8
__wrap___udivqi3:
pce_fast_udiv8:
    jsr .Ldiv8
    lda $2004
    rts
__wrap___umodqi3:
pce_fast_umod8:
    jsr .Ldiv8
    lda $2002
    rts
.Ldiv8:
    sta $2002
    stx $2003
    stz $2004
    txa
    beq 4f
    ldy #1
1:
    /* INY changes N: reload the shifted divisor before testing its bit 7. */
    lda $2003
    bmi 2f
    cmp $2002
    bcs 2f
    asl
    sta $2003
    iny
    bra 1b
2:
    sta $2003
3:
    asl $2004
    lda $2002
    cmp $2003
    bcc 5f
    sbc $2003
    sta $2002
    inc $2004
5:
    lsr $2003
    dey
    bne 3b
4:
    rts

.global __wrap___udivhi3, __wrap___umodhi3, __wrap___divhi3
.global pce_fast_udiv16, pce_fast_umod16, pce_fast_div16
__wrap___udivhi3:
pce_fast_udiv16:
    jsr .Ldiv16
    lda $2006
    ldx $2007
    rts
__wrap___umodhi3:
pce_fast_umod16:
    jsr .Ldiv16
    lda $2004
    ldx $2005
    rts
.Ldiv16:
    sta $2004
    stx $2005
    stz $2006
    stz $2007
    lda $2002
    ora $2003
    beq 4f
    ldy #1
1:
    lda $2003
    bmi 2f
    cmp $2005
    bcc 6f
    bne 2f
    lda $2002
    cmp $2004
    bcs 2f
6:
    asl $2002
    rol $2003
    iny
    bra 1b
2:
    asl $2006
    rol $2007
    lda $2004
    sec
    sbc $2002
    tax
    lda $2005
    sbc $2003
    bcc 3f
    sta $2005
    stx $2004
    inc $2006
3:
    lsr $2003
    ror $2002
    dey
    bne 2b
4:
    rts
__wrap___divhi3:
pce_fast_div16:
    sta $2004
    txa
    eor $2003
    pha
    txa
    bpl 1f
    sec
    cla
    sbc $2004
    sta $2004
    cla
    stx $2005
    sbc $2005
    tax
1:
    lda $2003
    bpl 2f
    sec
    cla
    sbc $2002
    sta $2002
    cla
    sbc $2003
    sta $2003
2:
    lda $2004
    jsr .Ldiv16
    pla
    bpl 3f
    sec
    cla
    sbc $2006
    sta $2006
    cla
    sbc $2007
    sta $2007
3:
    lda $2006
    ldx $2007
    rts

/* 32-bit restoring division; skip leading zero dividend bits. */
.global __wrap___udivsi3, pce_fast_udiv32
__wrap___udivsi3:
pce_fast_udiv32:
    sta $2008
    stx $2009
    lda $2002
    sta $200a
    lda $2003
    sta $200b
    stz $200c
    stz $200d
    stz $200e
    stz $200f
    ldy #32
    lda $2004
    ora $2005
    ora $2006
    ora $2007
    beq 4f
1:
    lda $200b
    bmi 2f
    asl $2008
    rol $2009
    rol $200a
    rol $200b
    dey
    bne 1b
    bra 4f
2:
    asl $2008
    rol $2009
    rol $200a
    rol $200b
    rol $200c
    rol $200d
    rol $200e
    rol $200f
    bcs 5f
    lda $200f
    cmp $2007
    bne 6f
    lda $200e
    cmp $2006
    bne 6f
    lda $200d
    cmp $2005
    bne 6f
    lda $200c
    cmp $2004
6:
    bcc 3f
5:
    sec
    lda $200c
    sbc $2004
    sta $200c
    lda $200d
    sbc $2005
    sta $200d
    lda $200e
    sbc $2006
    sta $200e
    lda $200f
    sbc $2007
    sta $200f
    inc $2008
3:
    dey
    bne 2b
    lda $200a
    sta $2002
    lda $200b
    sta $2003
    lda $2008
    ldx $2009
    rts
4:
    cla
    clx
    stz $2002
    stz $2003
    rts
/* The race owns cache_refs until video_scene returns it to the BG cache.
 * Unlike the general compiler helper, these byte planes are already mapped.
 * A * X -> A:X; $08,$09,$0c,$0d,Y clobbered. */
.extern cache_refs
.global race_umul8, race_mulw
race_umul8:
    sta $2008
    stx $2009
    clc
    adc $2009
    tay
    bcc 2f
    lda cache_refs+256,y
    sta $200c
    lda cache_refs+768,y
    bra 3f
2:
    lda cache_refs,y
    sta $200c
    lda cache_refs+512,y
3:
    sta $200d
    lda $2008
    sec
    sbc $2009
    bcs 1f
    eor #255
    inc
1:
    tay
    lda $200c
    sec
    sbc cache_refs,y
    pha
    lda $200d
    sbc cache_refs+512,y
    tax
    pla
    rts
/* Signed word by signed byte, exact long result (LLVM-MOS ABI).
 * Magnitudes use two byte products, then one sign correction. */
race_mulw:
    sta $2004
    stx $2005
    txa
    eor $2002
    sta $2007
    txa
    bpl 1f
    sec
    cla
    sbc $2004
    sta $2004
    cla
    sbc $2005
    sta $2005
1:
    lda $2002
    bpl 2f
    eor #255
    inc
2:
    sta $2006
    tax
    lda $2005
    pha
    lda $2004
    jsr race_umul8
    sta $2004
    stx $2005
    pla
    ldx $2006
    jsr race_umul8
    clc
    adc $2005
    sta $2005
    txa
    adc #0
    sta $2002
    stz $2003
    lda $2007
    bpl 3f
    sec
    cla
    sbc $2004
    sta $2004
    cla
    sbc $2005
    sta $2005
    cla
    sbc $2002
    sta $2002
    cla
    sbc $2003
    sta $2003
3:
    lda $2004
    ldx $2005
    rts
.global race_u8s8
race_u8s8:
    jsr race_umul8
    pha
    txa
    ldy $2009
    bpl 1f
    sec
    sbc $2008
1:
    tax
    pla
    rts
#endif
