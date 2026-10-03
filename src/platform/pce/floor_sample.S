/* Private scratch $20c0-$20d0. No LLVM imaginary registers are touched.
 * The row-major race map occupies Arcade bytes $000000-$0fffff. */
.section .ram_bank109.text,"ax",@progbits
.global floor_sample, floor_u, floor_v, floor_du, floor_dv, floor_phase
.extern floor_mip_lo, floor_mip_hi, floor_pairs
floor_u = 0x20c0
floor_v = 0x20c2
floor_du = 0x20c4
floor_dv = 0x20c6
floor_phase = 0x20d0
floor_sample:
    ldx #0
    stz $20ca
.Lsample:
    /* Ten-bit X cell; retain its low byte for both map and pursuit. */
    lda $20c0
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c8
    lda $20c1
    asl
    asl
    ora $20c8
    sta $20cb
    lda $20d0
    bne .Lpursuit
    lda $20cb
    sta $1a12
    lda $20c1
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c8
    lda $20c2
    lsr
    lsr
    lsr
    lsr
    and #12
    ora $20c8
    sta $20c8
    lda $20c3
    asl
    asl
    asl
    asl
    ora $20c8
    sta $1a13
    lda $20c3
    lsr
    lsr
    lsr
    lsr
    sta $1a14
    lda $1a10
    cmp #11
    bcc .Lmaterial
    lda #0
    bra .Lmaterial
.Lpursuit:
    lda $20c1
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    cmp #1
    beq .Lroadleft
    cmp #2
    bne .Lsand
    lda $20cb
    cmp #14
    bcs .Lsand
    cmp #0
    beq .Lstripe
    bra .Lasphalt
.Lroadleft:
    lda $20cb
    cmp #242
    bcc .Lsand
    cmp #255
    beq .Lstripe
.Lasphalt:
    lda #2
    bra .Lmaterial
.Lstripe:
    lda #3
    bra .Lmaterial
.Lsand:
    lda #0
.Lmaterial:
    tay
    lda floor_mip_lo,y
    sta $20cc
    lda floor_mip_hi,y
    sta $20cd
    lda $20c2
    lsr
    lsr
    and #56
    sta $20c8
    lda $20c0
    lsr
    lsr
    lsr
    lsr
    lsr
    ora $20c8
    tay
    lda ($20cc),y
    ldy $20ca
    bne .Llow
    asl
    asl
    asl
    asl
    sta $20c9
    inc $20ca
    bra .Ladvance
.Llow:
    ora $20c9
    sta floor_pairs,x
    inx
    stz $20ca
.Ladvance:
    clc
    lda $20c0
    adc $20c4
    sta $20c0
    lda $20c1
    adc $20c5
    sta $20c1
    clc
    lda $20c2
    adc $20c6
    sta $20c2
    lda $20c3
    adc $20c7
    sta $20c3
    cpx #64
    beq .Ldone
    jmp .Lsample
.Ldone:
    rts
