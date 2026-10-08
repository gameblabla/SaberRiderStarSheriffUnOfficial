/* Sprite fast path for cached 16x16-piece objects (no scaling, not the cockpit).
 * sprite_fast looks the object up in the resident cache, reads its piece list
 * from the Arcade Card with one block transfer, then emits SAT entries.
 * It reports sprite_fast_miss=1 when the C slow path must upload the object.
 *
 * Whole-object admission rolls back all band reservations on refusal.
 * Private scratch is $20c0-$20df, shared with the floor sampler; neither runs
 * from an IRQ and the two execute sequentially. LLVM's zero page is untouched.
 *
 * $c0/c1 SAT output pointer   $c2/c3 piece x base (flip: x-16)
 * $c4/c5 screen X             $c6/c7 screen Y
 * $c8/c9 pattern base         $ca    piece offset      $cb piece end
 * $cc    slot                 $cd    original SAT count
 * $ce    attribute low        $cf    flip (0 / 8)
 * $d0/d1 object Y             $d2/d3 object X          $d4/d5 pattern word */
.section .ram_bank116.text,"ax",@progbits
.global sprite_fast
.extern descriptor, sprite_emit_x, sprite_emit_y, sprite_emit_id, sprite_emit_flip
.extern sprite_emit_ok, sprite_fast_miss, video_nsprites
.extern sprite_line_lo, sprite_line_hi, sprite_line_ok
.extern sprite_lines_reserve, sprite_lines_release, sat_count, sat_page, sat, pce_raster_enabled, sprite_screen_height
.extern sprite_slot_of, sprite_ids, sprite_used, sprite_len, sprite_p0, sprite_p1, sprite_p2
.extern sprite_pb_hi, sprite_attr, sprite_words
#ifdef PCE_SGX
.extern pce_sgx_metrics
#endif
.extern arcade_tai, arcade_tai_src, arcade_tai_dst, arcade_tai_len
sprite_fast:
    bra .Lstart
.Lbail:
    rts
.Lstart:
    lda #1
    sta sprite_fast_miss
    stz sprite_emit_ok
    lda sprite_emit_id+1
    cmp video_nsprites+1
    bcc .Lrange
    bne .Lbail
    lda sprite_emit_id
    cmp video_nsprites
    bcs .Lbail
.Lrange:
    ldx sprite_emit_id
    lda sprite_emit_id+1
    bne .Lhigh
    lda sprite_slot_of,x
    bra .Lslot
.Lhigh:
    lda sprite_slot_of+256,x
.Lslot:
    cmp #48
    bcs .Lbail
    sta $20cc
    asl
    tax
    lda sprite_ids,x
    cmp sprite_emit_id
    bne .Lbail
    lda sprite_ids+1,x
    cmp sprite_emit_id+1
    bne .Lbail
    ldx $20cc
#ifdef PCE_SGX
    /* VDC1 has its own copy of this gameplay sprite cache. A VDC0 hit is
       still a miss until this slot has been uploaded there. */
    lda sat_page
    beq .Lvdc1_cache_ready
    lda pce_sgx_metrics+5
    and #0x80
    beq .Lvdc1_cache_ready
    lda sprite_pb_hi,x
    bpl .Lbail
.Lvdc1_cache_ready:
#endif
    lda #1
    sta sprite_used,x
    stz sprite_fast_miss
    /* Piece list: Arcade port 2 (registers $1a22-$1a24 hold the address). */
    lda sprite_p0,x
    sta $1a22
    lda sprite_p1,x
    sta $1a23
    lda sprite_p2,x
    sta $1a24
    lda #$20
    sta arcade_tai_src
    lda #$1a
    sta arcade_tai_src+1
    lda #<descriptor
    sta arcade_tai_dst
    lda #>descriptor
    sta arcade_tai_dst+1
    lda sprite_len,x
    sta arcade_tai_len
    sta $20cb
    stz arcade_tai_len+1
    jsr arcade_tai
    ldx $20cc
    txa
    asl
    tax
    lda sprite_words,x
    sta $20d4
    lda sprite_words+1,x
    sta $20d5
    lda $20d4
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c8
    lda $20d5
    and #$1f
    asl
    asl
    asl
    ora $20c8
    sta $20c8
    lda $20d5
    lsr
    lsr
    lsr
    lsr
    lsr
    sta $20c9
    ldx $20cc
    lda sprite_attr,x
    sta $20ce
    lda sprite_emit_flip
    sta $20cf
    /* SAT output pointer = sat + sat_count * 8. */
    lda sat_count
    sta $20cd
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
    lda sat_page
    beq .Lsat_page_ready
    clc
    lda $20c1
    adc #2
    sta $20c1
.Lsat_page_ready:
    lda sprite_emit_x
    sta $20d2
    sta $20c2
    lda sprite_emit_x+1
    sta $20d3
    sta $20c3
    lda sprite_emit_y
    sta $20d0
    lda sprite_emit_y+1
    sta $20d1
    lda $20cf
    beq .Lbegin
    sec
    lda $20c2
    sbc #16
    sta $20c2
    lda $20c3
    sbc #0
    sta $20c3
.Lbegin:
    stz $20ca
.Lpiece:
    ldx $20ca
    lda $20cf
    bne .Lflip
    clc
    lda descriptor,x
    adc $20d2
    sta $20c4
    lda descriptor+1,x
    adc $20d3
    bra .Lxdone
.Lflip:
    sec
    lda $20c2
    sbc descriptor,x
    sta $20c4
    lda $20c3
    sbc descriptor+1,x
.Lxdone:
    sta $20c5
    beq .Lyadd
    cmp #$ff
    bne .Lxwide
    lda $20c4
    cmp #$f1
    bcs .Lyadd
    jmp .Lskip
.Lxwide:
    cmp #1
    beq 1f
    jmp .Lskip
1:
    lda pce_raster_enabled
    bne .Lyadd
    jmp .Lskip
.Lyadd:
    clc
    lda descriptor+2,x
    adc $20d0
    sta $20c6
    lda descriptor+3,x
    adc $20d1
    sta $20c7
    bmi .Ltop
    beq 1f
    jmp .Lskip
1:
    lda $20c6
    cmp sprite_screen_height
    bcc 1f
    jmp .Lskip
1:
    sta sprite_line_lo
    clc
    adc #16
    cmp sprite_screen_height
    bcc .Lhi
    lda sprite_screen_height
    bra .Lhi
.Ltop:
    cmp #$ff
    beq 1f
    jmp .Lskip
1:
    lda $20c6
    cmp #$f1
    bcs 1f
    jmp .Lskip
1:
    stz sprite_line_lo
    clc
    adc #16
.Lhi:
    sta sprite_line_hi
    lda sat_count
    cmp #64
    bcs .Lrefuse
    jsr sprite_lines_reserve
    lda sprite_line_ok
    beq .Lrefuse
    ldx $20ca
    lda descriptor+4,x
    asl
    clc
    adc $20c8
    sta $20d4
    lda $20c9
    adc #0
    sta $20d5
    ldy #0
    clc
    lda $20c6
    adc #64
    sta ($20c0),y
    iny
    lda $20c7
    adc #0
    sta ($20c0),y
    iny
    clc
    lda $20c4
    adc #32
    sta ($20c0),y
    iny
    lda $20c5
    adc #0
    sta ($20c0),y
    iny
    lda $20d4
    sta ($20c0),y
    iny
    lda $20d5
    sta ($20c0),y
    iny
    lda $20ce
    sta ($20c0),y
    iny
    lda $20cf
    sta ($20c0),y
    clc
    lda $20c0
    adc #8
    sta $20c0
    bcc .Lcount
    inc $20c1
.Lcount:
    inc sat_count
.Lskip:
    clc
    lda $20ca
    adc #6
    sta $20ca
    cmp $20cb
    bcs .Lok
    jmp .Lpiece
.Lok:
    inc sprite_emit_ok
    rts
/* Roll back the entries this object added, releasing their bands. */
.Lrefuse:
    lda sat_count
    cmp $20cd
    bne 1f
    rts
1:
    sec
    lda $20c0
    sbc #8
    sta $20c0
    bcs .Lrewind
    dec $20c1
.Lrewind:
    dec sat_count
    ldy #0
    sec
    lda ($20c0),y
    sbc #64
    sta $20c6
    iny
    lda ($20c0),y
    sbc #0
    bmi .Lrtop
    lda $20c6
    sta sprite_line_lo
    clc
    adc #16
    cmp sprite_screen_height
    bcc .Lrhi
    lda sprite_screen_height
    bra .Lrhi
.Lrtop:
    stz sprite_line_lo
    clc
    lda $20c6
    adc #16
.Lrhi:
    sta sprite_line_hi
    jsr sprite_lines_release
    bra .Lrefuse
