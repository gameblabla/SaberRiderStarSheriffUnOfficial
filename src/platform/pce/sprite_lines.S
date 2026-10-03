/* Scanline admission. Platform stages use 8-line bands; the other stages (race,
 * cockpit, space) count exact scanlines because their sprites stack densely.
 * Bands: A part touching a band counts against
 * every band it overlaps, so each scanline's true occupancy never exceeds its
 * band's count: the limit of 16 sprites per scanline is never exceeded, at
 * the cost of occasionally refusing a part an exact count would admit.
 * All scratch is private RAM; IRQs never use these arrays or registers. */
.section .ram_bank106.text,"ax",@progbits
.global sprite_lines_reserve, sprite_lines_release
.extern sprite_occupancy, sprite_line_lo, sprite_line_hi, sprite_line_ok, sprite_exact
.comm sprite_band_last,1
sprite_lines_reserve:
    stz sprite_line_ok
    lda sprite_exact
    beq 1f
    jmp .Lexact_reserve
1:
    lda sprite_line_hi
    dec
    lsr
    lsr
    lsr
    sta sprite_band_last
    lda sprite_line_lo
    lsr
    lsr
    lsr
    tax
    tay
.Lcheck:
    lda sprite_occupancy,x
    cmp #16
    bcs .Ldone
    cpx sprite_band_last
    beq .Ladd
    inx
    bra .Lcheck
.Ladd:
    tya
    tax
.Lbump:
    inc sprite_occupancy,x
    cpx sprite_band_last
    beq .Lok
    inx
    bra .Lbump
.Lok:
    inc sprite_line_ok
.Ldone:
    rts
sprite_lines_release:
    lda sprite_exact
    beq 1f
    jmp .Lexact_release
1:
    lda sprite_line_hi
    dec
    lsr
    lsr
    lsr
    sta sprite_band_last
    lda sprite_line_lo
    lsr
    lsr
    lsr
    tax
.Lsub:
    dec sprite_occupancy,x
    cpx sprite_band_last
    beq .Lout
    inx
    bra .Lsub
.Lout:
    rts
.Lexact_reserve:
    ldx sprite_line_lo
.Lxcheck:
    lda sprite_occupancy,x
    cmp #16
    bcs .Lxdone
    inx
    cpx sprite_line_hi
    bcc .Lxcheck
    ldx sprite_line_lo
.Lxadd:
    inc sprite_occupancy,x
    inx
    cpx sprite_line_hi
    bcc .Lxadd
    inc sprite_line_ok
.Lxdone:
    rts
.Lexact_release:
    ldx sprite_line_lo
.Lxsub:
    dec sprite_occupancy,x
    inx
    cpx sprite_line_hi
    bcc .Lxsub
    rts

/* Re-emit the retained foreground parts. Records are FgPart (14 bytes): the
 * first eight are a SAT entry whose X is the world-space X word. Parts that
 * are off-screen or whose bands are full are skipped (scenery is optional).
 * Scratch: $d8/d9 record pointer, $da remaining, $db last band, $c0/c1 SAT out,
 * $c4/c5 screen X word. */
.global fg_emit
.extern fg_parts, fg_count, fg_camera, sat, sat_count, sprite_used
fg_emit:
    lda fg_count
    bne 1f
    rts
1:
    sta $20da
    lda #<fg_parts
    sta $20d8
    lda #>fg_parts
    sta $20d9
    lda sat_count
    stz $20c1
    asl
    rol $20c1
    asl
    rol $20c1
    asl
    rol $20c1
    clc
    adc #<sat
    sta $20c0
    lda $20c1
    adc #>sat
    sta $20c1
.Lfpart:
    ldy #10
    lda ($20d8),y
    tax
    lda #1
    sta sprite_used,x
    ldy #2
    sec
    lda ($20d8),y
    sbc fg_camera
    sta $20c4
    iny
    lda ($20d8),y
    sbc fg_camera+1
    sta $20c5
    beq .Lflow
    cmp #1
    bne .Lfskip
    lda $20c4
    cmp #32
    bcs .Lfskip
    bra .Lfvisible
.Lflow:
    lda $20c4
    cmp #17
    bcc .Lfskip
.Lfvisible:
    lda sat_count
    cmp #64
    bcs .Lfdone
    ldy #9
    lda ($20d8),y
    sta $20db
    dey
    lda ($20d8),y
    tax
.Lfcheck:
    lda sprite_occupancy,x
    cmp #16
    bcs .Lfskip
    cpx $20db
    beq 1f
    inx
    bra .Lfcheck
1:
    ldy #8
    lda ($20d8),y
    tax
.Lfbump:
    inc sprite_occupancy,x
    cpx $20db
    beq 1f
    inx
    bra .Lfbump
1:
    ldy #0
    lda ($20d8),y
    sta ($20c0),y
    iny
    lda ($20d8),y
    sta ($20c0),y
    iny
    lda $20c4
    sta ($20c0),y
    iny
    lda $20c5
    sta ($20c0),y
    iny
    lda ($20d8),y
    sta ($20c0),y
    iny
    lda ($20d8),y
    sta ($20c0),y
    iny
    lda ($20d8),y
    sta ($20c0),y
    iny
    lda ($20d8),y
    sta ($20c0),y
    clc
    lda $20c0
    adc #8
    sta $20c0
    bcc 1f
    inc $20c1
1:
    inc sat_count
.Lfskip:
    clc
    lda $20d8
    adc #14
    sta $20d8
    bcc 1f
    inc $20d9
1:
    dec $20da
    beq .Lfdone
    jmp .Lfpart
.Lfdone:
    rts
