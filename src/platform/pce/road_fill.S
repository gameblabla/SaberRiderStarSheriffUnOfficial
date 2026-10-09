/* Primitives of the road builder (road_pce.c). Private scratch is the zero page $20c0-$20df shared with the sprite emitters, which
 * never run while the road is built; arguments are globals of road_pce.c.
 *
 * road_fill     (bank $6d) writes road_fill_n table entries, from index road_fill_m downwards. The BXR of a line is the middle two
 *               bytes of the 24-bit value road_fill_b (a dot is 256, the top byte is ignored: the VDC keeps ten bits), which moves
 *               by the 24-bit road_fill_s on every line. The low bytes go to road_fill_base + m, the high ones to +256.
 * road_divide   (bank $6d) road_div_q = road_div_q / road_div_n (16 by 8 bits, unsigned).
 * road_stripes  (bank $6d) the picture copy of every scanline, 0 or 1 from bit 5 of (road_along + the scanline's distance, low byte), plus 2 in
 *               the lower rows when the line's window wraps past the picture's right edge, 4 past its left (see build_assets.py road_assets).
 * road_advance  (bank $6f) road_k[0] += dx c + dy s, road_k[1] += dy c - dx s, in Q14 (A = dx, X = dy; c = 128 cam_c + cam_cl, s = 128 cam_s
 *               + cam_sl: the camera's sine and cosine to 14 bits, as a byte and a small remainder).
 * smul8         (bank $6f) the signed 16-bit product of two signed bytes (A, X -> A low, X high). */
.section .ram_bank109.text,"ax",@progbits
.global road_fill, road_divide, road_stripes
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
.extern road_sel_base, road_along, pce_road_z
road_stripes:
    lda road_sel_base
    sta 0x20c0
    sta 0x20c2
    lda road_sel_base+1
    sta 0x20c1
    dec
    sta 0x20c3          /* the high bytes of the lines' BXR are the page before */
    cla
    ldy #0
1:
    sta (0x20c0),y
    iny
    cpy #24
    bne 1b
2:
    lda pce_road_z-1,y
    clc
    adc road_along
    and #32
    beq 3f
    lda #1
3:
    tax                 /* the stripe copy: 0 or 1 */
    cpy #64
    bcc 5f
    lda (0x20c2),y      /* the lower rows: a window past the picture's right edge (BXR 512..1023 or more) or its left (negative) */
    bmi 4f
    cmp #2
    bcc 5f
    inx
    inx
    bra 5f
4:
    inx
    inx
    inx
    inx
5:
    txa
    sta (0x20c0),y
    iny
    cpy #112
    bne 2b
    rts

.section .ram_bank111.text,"ax",@progbits
.global smul8, road_advance
.extern cam_c, cam_s, cam_cl, cam_sl, road_k
T0 = 0x20d8
T1 = 0x20d9
T2 = 0x20da
T3 = 0x20c4
T4 = 0x20c5
T5 = 0x20c6
T6 = 0x20c7
SL = 0x20db
SH = 0x20dc
DX = 0x20dd
DY = 0x20de
/* q(a+b)-q(|a-b|), with magnitudes 0..128. The table is initialized
 * before race_start and remains resident in the race's borrowed BG cache. */
.extern cache_refs
smul8:
    sta T0
    txa
    eor T0
    sta T2
    txa
    bpl 1f
    eor #$ff
    inc
1:
    sta T1
    lda T0
    bpl 1f
    eor #$ff
    inc
1:
    sta T0
    phy
    clc
    adc T1
    ldy #0
    sty T4
    rol T4
    asl
    rol T4
    clc
    adc #<cache_refs
    sta T3
    lda T4
    adc #>cache_refs
    sta T4
    lda (T3),y
    sta T5
    iny
    lda (T3),y
    sta T6
    sec
    lda T0
    sbc T1
    bcs 1f
    eor #$ff
    inc
1:
    stz T4
    asl
    rol T4
    clc
    adc #<cache_refs
    sta T3
    lda T4
    adc #>cache_refs
    sta T4
    ldy #0
    sec
    lda T5
    sbc (T3),y
    sta T0
    iny
    lda T6
    sbc (T3),y
    sta T1
    lda T2
    bpl 1f
    sec
    cla
    sbc T0
    sta T0
    cla
    sbc T1
    sta T1
1:
    ply
    lda T0
    ldx T1
    rts
/* road_k[X/4] += SH:SL << 7 (X = 0 or 4); the shift is the 24-bit SH:SL:0 arithmetic-shifted right once */
.Lacc7:
    lda SH
    cmp #0x80
    ror
    sta T3
    lda SL
    ror
    sta T4
    cla
    ror
    sta T5
    lda SH
    asl
    cla
    bcc 1f
    lda #0xff
1:
    sta T6
    clc
    lda T5
    adc road_k,x
    sta road_k,x
    lda T4
    adc road_k+1,x
    sta road_k+1,x
    lda T3
    adc road_k+2,x
    sta road_k+2,x
    lda T6
    adc road_k+3,x
    sta road_k+3,x
    rts
/* road_k[X/4] += SH:SL (signed) */
.Lacc0:
    lda SH
    asl
    cla
    bcc 1f
    lda #0xff
1:
    sta T6
    clc
    lda SL
    adc road_k,x
    sta road_k,x
    lda SH
    adc road_k+1,x
    sta road_k+1,x
    lda T6
    adc road_k+2,x
    sta road_k+2,x
    lda T6
    adc road_k+3,x
    sta road_k+3,x
    rts
/* SH:SL = A1*M1 + A2*M2 or A1*M1 - A2*M2 (set = clc, op = adc, or sec, sbc); the A's are zero-page bytes, the M's globals */
.macro DOT2 a1, m1, a2, m2, set, op
    lda \a1
    ldx \m1
    jsr smul8
    sta SL
    stx SH
    lda \a2
    ldx \m2
    jsr smul8
    sta T0
    stx T1
    \set
    lda SL
    \op T0
    sta SL
    lda SH
    \op T1
    sta SH
.endm
road_advance:
    sta DX
    stx DY
    DOT2 DX, cam_c, DY, cam_s, clc, adc
    ldx #0
    jsr .Lacc7
    DOT2 DX, cam_cl, DY, cam_sl, clc, adc
    ldx #0
    jsr .Lacc0
    DOT2 DY, cam_c, DX, cam_s, sec, sbc
    ldx #4
    jsr .Lacc7
    DOT2 DY, cam_cl, DX, cam_sl, sec, sbc
    ldx #4
    jmp .Lacc0
