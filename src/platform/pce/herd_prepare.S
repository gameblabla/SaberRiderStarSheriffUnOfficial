/* Collect visible horse columns once. Actor is 21 bytes; records are six
 * bytes (screen X, screen Y, first/last column). All scratch is private ZP. */
.section .ram_bank110.text,"ax",@progbits
.global herd_prepare
.extern actors, camera, herd_live, herd_horse_count, herd_cells, herd_same, herd_horses
herd_prepare:
    stz herd_live
    stz herd_horse_count
    stz herd_cells
    lda #1
    sta herd_same
    ldx #0
    ldy #0
.Lactor:
    lda actors+12,x
    beq .Lnext_near
    lda actors+13,x
    cmp #11
    beq .Lhorse
.Lnext_near:
    jmp .Lnext
.Lhorse:
    inc herd_live
    sec
    lda actors,x
    sbc camera
    sta $20c4
    lda actors+1,x
    sbc camera+1
    sta $20c5
    sec
    lda $20c4
    sbc #72
    sta $20c4
    lda $20c5
    sbc #0
    sta $20c5
    beq .Lpositive
    cmp #$ff
    bne .Lnext_near
    lda $20c4
    cmp #129
    bcc .Lnext_near
    eor #$ff
    inc
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c2
    lda #4
    sta $20c3
    bra .Ly
.Lpositive:
    stz $20c2
    lda #4
    sta $20c3
    lda $20c4
    cmp #160
    bcc .Ly
    sec
    lda #31
    sbc $20c4
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c3
.Ly:
    sec
    lda actors+2,x
    sbc #48
    sta $20c6
    lda actors+3,x
    sbc #0
    sta $20c7
    lda herd_horse_count
    beq .Lstore
    lda $20c6
    cmp herd_horses+2
    bne .Ldifferent
    lda $20c7
    cmp herd_horses+3
    beq .Lstore
.Ldifferent:
    stz herd_same
.Lstore:
    lda $20c4
    sta herd_horses,y
    lda $20c5
    sta herd_horses+1,y
    lda $20c6
    sta herd_horses+2,y
    lda $20c7
    sta herd_horses+3,y
    lda $20c2
    sta herd_horses+4,y
    lda $20c3
    sta herd_horses+5,y
    sec
    sbc $20c2
    clc
    adc herd_cells
    sta herd_cells
    inc herd_horse_count
    tya
    clc
    adc #6
    tay
.Lnext:
    txa
    clc
    adc #21
    tax
    cpx #168
    beq 1f
    jmp .Lactor
1:
    rts
