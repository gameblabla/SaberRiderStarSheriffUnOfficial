#ifdef PCE_SGX
/* Render only the 48 pairs consumed by irq.S. Scratch $c0-$df belongs to
 * road construction until the subsequent sprite draw. No IRQ masks or bank
 * changes: raster and PCM deadlines remain unchanged. */
.section .ram_bank128.text,"ax",@progbits
.global road_baked_fill
.extern columns,cache_refs,road_fill_base,road_baked_frac,road_baked_bias,road_baked_step,rphase
DA = $20c4
DH = $20c5
PL = $20c6
PH = $20c7
FR = $20c8
BI0 = $20c9
BI1 = $20ca
BI2 = $20cb
BL = $20cf
BH = $20d0
SX = $20d1
road_baked_fill:
    lda road_fill_base
    sta $20c0
    sta $20c2
    lda road_fill_base+1
    sta $20c1
    inc
    sta $20c3
    lda road_baked_bias
    sta BI0
    lda road_baked_bias+1
    sta BI1
    lda road_baked_bias+2
    sta BI2
    ldy #16
    ldx #0
.Lcurve:
    stx SX
    lda rphase
    cmp #3
    bcc 1f
    stz PL
    stz PH
    jmp .Lbias
1:
    lda columns+1792,x
    sec
    sbc columns+1664,x
    sta DA
    lda columns+1793,x
    sbc columns+1665,x
    sta DH
    ora DA
    beq .Lconstant
    lda road_baked_frac
    bne .Linterpolate
.Lconstant:
    lda columns+1664,x
    sta PL
    lda columns+1665,x
    sta PH
    jmp .Lbias
.Linterpolate:
    sta FR
    lda DH
    bpl 1f
    lda DA
    eor #255
    inc
    sta DA
1:
    /* Adjacent pixel centres differ by at most 127 (bake assertion).
       A resident quarter-square product replaces the nibble shift loop. */
    lda DA
    clc
    adc FR
    tax
    lda cache_refs,x
    sta PL
    lda cache_refs+512,x
    sta PH
    lda DA
    sec
    sbc FR
    bcs 1f
    eor #255
    inc
1:
    tax
    sec
    lda PL
    sbc cache_refs,x
    sta PL
    lda PH
    sbc cache_refs+512,x
    sta PH
    lda DH
    bpl 1f
    sec
    cla
    sbc PL
    sta PL
    cla
    sbc PH
    sta PH
1:
    /* Arithmetic >>4. The interpolated delta itself fits a signed byte. */
    lda PH
    asl
    asl
    asl
    asl
    sta DH
    lda PL
    lsr
    lsr
    lsr
    lsr
    ora DH
    sta PL
    asl
    cla
    bcc 1f
    lda #255
1:
    sta PH
    ldx SX
    clc
    lda PL
    adc columns+1664,x
    sta PL
    lda PH
    adc columns+1665,x
    sta PH
.Lbias:
    /* Pixel centre subtraction needs only the middle bytes of Q8 bias. */
    sec
    lda BI1
    sbc PL
    sta BL
    lda BI2
    sbc PH
    sta BH
    /* Preserve the frozen far endpoint above its last knot. Clamp only
       to the static picture's safe windows; a d-dependent side cap would
       pull a terminating bend inward through the haze. */
    ldx #0
    cpy #66
    bcc 1f
    inx
1:
    lda BH
    eor #128
    cmp .Lminimum_hi,x
    bcc .Lminimum
    bne .Lcheck_maximum
    lda BL
    cmp .Lminimum_lo,x
    bcc .Lminimum
.Lcheck_maximum:
    lda BH
    eor #128
    cmp .Lmaximum_hi,x
    bcc .Lstore
    bne .Lmaximum
    lda BL
    cmp .Lmaximum_lo,x
    bcc .Lstore
    beq .Lstore
.Lmaximum:
    lda .Lmaximum_lo,x
    sta BL
    lda .Lmaximum_hi,x
    eor #128
    sta BH
    bra .Lstore
.Lminimum:
    lda .Lminimum_lo,x
    sta BL
    lda .Lminimum_hi,x
    eor #128
    sta BH
.Lstore:
    cpy #16
    bne 3f
    lda BL
    sta $20d2
    lda BH
    sta $20d3
3:
    lda BL
    sta ($20c0),y
    iny
    sta ($20c0),y
    lda BH
    sta ($20c2),y
    dey
    sta ($20c2),y
    iny
    iny
    clc
    lda BI0
    adc road_baked_step
    sta BI0
    lda BI1
    adc road_baked_step+1
    sta BI1
    lda road_baked_step+1
    bmi 1f
    cla
    bra 2f
1:
    lda #255
2:
    adc BI2
    sta BI2
    ldx SX
    inx
    inx
    cpy #112
    beq .Ldone
    jmp .Lcurve
.Ldone:
    /* Unused haze entries still get a deterministic centre, for inspection
       and dialogue restoration. */
    ldy #15
1:
    lda $20d2
    sta ($20c0),y
    lda $20d3
    sta ($20c2),y
    dey
    bpl 1b
    rts
.section .ram_bank128.rodata,"a",@progbits
.Lminimum_lo:
    .byte 32,224
.Lminimum_hi:
    .byte 127,125
.Lmaximum_lo:
    .byte 224,32
.Lmaximum_hi:
    .byte 130,132
#endif
