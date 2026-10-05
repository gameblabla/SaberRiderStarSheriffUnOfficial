/* Primitives of the road builder (road_pce.c). Private scratch is the zero page $20c0-$20df shared with the sprite emitters, which
 * never run while the road is built; arguments are globals of road_pce.c.
 *
 * road_fill     (bank $6d) writes road_fill_n table entries, from index road_fill_m downwards. The BXR of a line is the middle two
 *               bytes of the 24-bit value road_fill_b (a dot is 256, the top byte is ignored: the VDC keeps ten bits), which moves
 *               by the 24-bit road_fill_s on every line. The low bytes go to road_fill_base + m, the high ones to +256.
 * road_divide   (bank $6d) road_div_q = road_div_q / road_div_n (16 by 8 bits, unsigned).
 * road_stripes  (bank $6f) the stripe copy of every scanline: bit 5 of (road_along + the scanline's distance, low byte).
 * road_advance  (bank $6f) knot_f += (dx c + dy s + 64) >> 7, knot_side += (dy c - dx s + 64) >> 7 (A = dx, X = dy; c, s = cam_c, cam_s).
 * smul8         (bank $6f) the signed 16-bit product of two signed bytes (A, X -> A low, X high). */
.section .ram_bank109.text,"ax",@progbits
.global road_fill, road_divide
.extern road_fill_base, road_fill_b0, road_fill_b1, road_fill_b2, road_fill_s0, road_fill_s1, road_fill_s2
.extern road_fill_m, road_fill_n, road_div_q, road_div_n
road_fill:
    lda road_fill_n
    beq .Lfill_done
    lda road_fill_base
    sta 0x20c0
    sta 0x20c2
    lda road_fill_base+1
    sta 0x20c1
    inc
    sta 0x20c3
    ldy road_fill_m
    ldx road_fill_n
.Lfill_line:
    lda road_fill_b1
    sta (0x20c0),y
    lda road_fill_b2
    sta (0x20c2),y
    lda road_fill_b0
    clc
    adc road_fill_s0
    sta road_fill_b0
    lda road_fill_b1
    adc road_fill_s1
    sta road_fill_b1
    lda road_fill_b2
    adc road_fill_s2
    sta road_fill_b2
    dey
    dex
    bne .Lfill_line
.Lfill_done:
    rts
road_divide:
    cla
    ldx #16
.Ldivide_bit:
    asl road_div_q
    rol road_div_q+1
    rol
    cmp road_div_n
    bcc 1f
    sbc road_div_n
    inc road_div_q
1:
    dex
    bne .Ldivide_bit
    rts

.section .ram_bank111.text,"ax",@progbits
.global smul8, road_stripes, road_advance
.extern road_sel_base, road_along, pce_road_z, cam_c, cam_s, knot_f, knot_side
T0 = 0x20d8
T1 = 0x20d9
T2 = 0x20da
SL = 0x20db
SH = 0x20dc
DX = 0x20dd
DY = 0x20de
smul8:
    sta T0
    stx T1
    stx T2
    cla
    ldx #8
    lsr T1
.Lmul_bit:
    bcc 1f
    clc
    adc T0
1:
    ror
    ror T1
    dex
    bne .Lmul_bit
    ldx T0
    bpl 2f
    sec
    sbc T2
2:
    ldx T2
    bpl 3f
    sec
    sbc T0
3:
    tax
    lda T1
    rts
road_stripes:
    lda road_sel_base
    sta 0x20c0
    lda road_sel_base+1
    sta 0x20c1
    cla
    ldy #0
1:
    sta (0x20c0),y
    iny
    cpy #16
    bne 1b
2:
    lda pce_road_z-1,y
    clc
    adc road_along
    and #32
    sta (0x20c0),y
    iny
    cpy #112
    bne 2b
    rts
/* S = A*B (16 bits, in SL:SH after the call below) */
road_advance:
    sta DX
    stx DY
    lda DX
    ldx cam_c
    jsr smul8
    sta SL
    stx SH
    lda DY
    ldx cam_s
    jsr smul8
    clc
    adc SL
    sta SL
    txa
    adc SH
    sta SH
    jsr .Lscale
    clc
    adc knot_f
    sta knot_f
    txa
    adc knot_f+1
    sta knot_f+1
    lda DY
    ldx cam_c
    jsr smul8
    sta SL
    stx SH
    lda DX
    ldx cam_s
    jsr smul8
    sta T0
    stx T1
    sec
    lda SL
    sbc T0
    sta SL
    lda SH
    sbc T1
    sta SH
    jsr .Lscale
    clc
    adc knot_side
    sta knot_side
    txa
    adc knot_side+1
    sta knot_side+1
    rts
/* (S + 64) >> 7 as a signed 16-bit value in X:A (|S| + 64 < 32768) */
.Lscale:
    lda SL
    clc
    adc #64
    sta SL
    lda SH
    adc #0
    asl SL
    rol
    ldx #0
    bcc 1f
    ldx #0xff
1:
    rts
