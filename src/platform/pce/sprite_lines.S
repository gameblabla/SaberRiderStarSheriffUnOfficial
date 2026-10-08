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
.comm sprite_line_cells,1
.comm sprite_line_add,1
.comm sprite_wide_left,1
.comm sprite_wide_span,1
.comm sprite_last_lo,1
.comm sprite_last_free,1

#ifdef PCE_SGX
/* IRQs never use admission state. Select a separate budget by changing
 * absolute operand pages; both arrays have the same low address. */
.comm sprite_lines_hi,1
.macro occupancy op, offset=0
.Loccupancy\@:
    \op sprite_occupancy+\offset,x
    .pushsection .ram_bank110.text.select,"ax",@progbits
    sta .Loccupancy\@+2
    .popsection
.endm
.pushsection .ram_bank110.text.select,"ax",@progbits
.global sprite_lines_select
sprite_lines_select:
    lda sprite_lines_hi
.popsection
#endif

/* Admit up to sprite_line_cells 32-wide cells with an identical Y span.
 * Computing the maximum once gives exactly the same admissions as repeated
 * reserve/reserve/release, including partially occupied edge scanlines. */
.global sprite_lines_wide, sprite_line_cells
.section .ram_bank110.text,"ax",@progbits
sprite_lines_wide:
    stz sprite_last_free
    lda sprite_line_hi
    dec
    ldx sprite_exact
    bne 1f
    lsr
    lsr
    lsr
1:
    sta sprite_band_last
    lda sprite_line_lo
    ldx sprite_exact
    bne 1f
    lsr
    lsr
    lsr
1:
    tax
    tay
    /* In band mode X is the divided start, not sprite_line_lo. */
    stx sprite_wide_span
    sec
    lda sprite_band_last
    sbc sprite_wide_span
    inc
    sta sprite_wide_span
    sta sprite_wide_left
    cla
.Lwmax:
    ldx sprite_wide_left
    cpx #4
    bcc .Lwmax_tail
    pha
    tya
    tax
    pla
    .rept 4
#ifdef PCE_SGX
    occupancy cmp, 0
#else
        cmp sprite_occupancy,x
#endif
        bcs 1f
#ifdef PCE_SGX
    occupancy lda, 0
#else
        lda sprite_occupancy,x
#endif
1:
        inx
    .endr
    pha
    txa
    tay
    lda sprite_wide_left
    sec
    sbc #4
    sta sprite_wide_left
    pla
    bra .Lwmax
.Lwmax_tail:
    ldx sprite_wide_left
    beq .Lwcapacity
    pha
    tya
    tax
    pla
.Lwmax_one:
#ifdef PCE_SGX
    occupancy cmp, 0
#else
    cmp sprite_occupancy,x
#endif
    bcs 1f
#ifdef PCE_SGX
    occupancy lda, 0
#else
    lda sprite_occupancy,x
#endif
1:
    inx
    dec sprite_wide_left
    bne .Lwmax_one
.Lwcapacity:
    /* A is at most 16; carry was changed by CPX, so set it explicitly. */
    eor #$ff
    clc
    adc #17
    lsr
    cmp sprite_line_cells
    bcc 1f
    lda sprite_line_cells
1:
    sta sprite_line_cells
    beq .Lwout
    asl
    sta sprite_line_add
    lda sprite_line_lo
    ldx sprite_exact
    bne 1f
    lsr
    lsr
    lsr
1:
    tax
    lda sprite_wide_span
    sta sprite_wide_left
.Lwadd:
    lda sprite_wide_left
    cmp #4
    bcc .Lwadd_tail
    .set line, 0
    .rept 4
#ifdef PCE_SGX
    occupancy lda, line
#else
        lda sprite_occupancy+line,x
#endif
        clc
        adc sprite_line_add
#ifdef PCE_SGX
    occupancy sta, line
#else
        sta sprite_occupancy+line,x
#endif
        .set line, line+1
    .endr
    inx
    inx
    inx
    inx
    lda sprite_wide_left
    sec
    sbc #4
    sta sprite_wide_left
    bra .Lwadd
.Lwadd_tail:
    lda sprite_wide_left
    beq .Lwout
.Lwadd_one:
#ifdef PCE_SGX
    occupancy lda, 0
#else
    lda sprite_occupancy,x
#endif
    clc
    adc sprite_line_add
#ifdef PCE_SGX
    occupancy sta, 0
#else
    sta sprite_occupancy,x
#endif
    inx
    dec sprite_wide_left
    bne .Lwadd_one
.Lwout:
    rts
/* Called directly from several different MPR3 overlays. Keep admission in
 * the always-mapped renderer bank, just like the exact sixteen-line helper. */
.section .ram_bank106.text,"ax",@progbits
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
#ifdef PCE_SGX
    occupancy lda, 0
#else
    lda sprite_occupancy,x
#endif
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
#ifdef PCE_SGX
    occupancy inc, 0
#else
    inc sprite_occupancy,x
#endif
    cpx sprite_band_last
    beq .Lok
    inx
    bra .Lbump
.Lok:
    inc sprite_line_ok
.Ldone:
    rts
sprite_lines_release:
    stz sprite_last_free
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
#ifdef PCE_SGX
    occupancy dec, 0
#else
    dec sprite_occupancy,x
#endif
    cpx sprite_band_last
    beq .Lout
    inx
    bra .Lsub
.Lout:
    rts
.Lexact_reserve:
    lda sprite_line_hi
    sec
    sbc sprite_line_lo
    cmp #16
    bne 1f
    jmp sprite_lines_sixteen
1:
    stz sprite_last_free
    ldx sprite_line_lo
.Lxcheck:
#ifdef PCE_SGX
    occupancy lda, 0
#else
    lda sprite_occupancy,x
#endif
    cmp #16
    bcs .Lxdone
    inx
    cpx sprite_line_hi
    bcc .Lxcheck
    ldx sprite_line_lo
.Lxadd:
#ifdef PCE_SGX
    occupancy inc, 0
#else
    inc sprite_occupancy,x
#endif
    inx
    cpx sprite_line_hi
    bcc .Lxadd
    inc sprite_line_ok
.Lxdone:
    rts
.Lexact_release:
    ldx sprite_line_lo
.Lxsub:
#ifdef PCE_SGX
    occupancy dec, 0
#else
    dec sprite_occupancy,x
#endif
    inx
    cpx sprite_line_hi
    bcc .Lxsub
    rts

/* Clear four lines per branch, with normal interruptible stores so the
 * timer's sample deadline is unaffected. Exact scanline counts are retained. */
.section .ram_bank107.text,"ax",@progbits
.global sprite_lines_sixteen
/* Ordinary sprite pieces cover exactly sixteen scanlines. Eliminate two
 * counted loops while keeping the check-before-commit transaction. */
sprite_lines_sixteen:
    ldx sprite_line_lo
    cpx sprite_last_lo
    bne .Lsixteen_scan
    lda sprite_last_free
    beq .Lsixteen_scan
    dec sprite_last_free
    jmp .Lsixteen_add
.Lsixteen_scan:
    cla
    .set line, 0
    .rept 16
#ifdef PCE_SGX
    occupancy cmp, line
#else
        cmp sprite_occupancy+line,x
#endif
        bcs 1f
#ifdef PCE_SGX
    occupancy lda, line
#else
        lda sprite_occupancy+line,x
#endif
1:
        .set line, line+1
    .endr
    cmp #16
    bcs .Lsixteen_full
    eor #$ff
    clc
    adc #17
    dec
    sta sprite_last_free
    stx sprite_last_lo
    bra .Lsixteen_add
.Lsixteen_full:
    rts
.Lsixteen_add:
    .set line, 0
    .rept 16
#ifdef PCE_SGX
    occupancy inc, line
#else
        inc sprite_occupancy+line,x
#endif
        .set line, line+1
    .endr
    inc sprite_line_ok
    rts
.section .text.irq,"ax",@progbits
.global sprite_lines_clear
sprite_lines_clear:
    stz sprite_last_free
    ldx #31
    lda sprite_exact
    beq 1f
    ldx #239
1:
.Lclear:
#ifdef PCE_SGX
    occupancy stz, 0
#else
    stz sprite_occupancy,x
#endif
    dex
#ifdef PCE_SGX
    occupancy stz, 0
#else
    stz sprite_occupancy,x
#endif
    dex
#ifdef PCE_SGX
    occupancy stz, 0
#else
    stz sprite_occupancy,x
#endif
    dex
#ifdef PCE_SGX
    occupancy stz, 0
#else
    stz sprite_occupancy,x
#endif
    dex
    cpx #255
    bne .Lclear
    rts

#ifdef PCE_SGX
.pushsection .ram_bank110.text.select,"ax",@progbits
    rts
.popsection
#endif
.section .ram_bank116.text,"ax",@progbits

/* Re-emit the retained foreground parts. Records are FgPart (14 bytes): the
 * first eight are a SAT entry whose X is the world-space X word. Parts that
 * are off-screen or whose bands are full are skipped (scenery is optional).
 * Scratch: $d8/d9 record pointer, $da remaining, $db last band, $c0/c1 SAT out,
 * $c4/c5 screen X word. */
.global fg_emit
.extern fg_parts, fg_count, fg_camera, fg_camera_slow, sat, sat_page, sat_count, sprite_used
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
    lda sat_page
    beq .Lfbase
    clc
    lda $20c1
    adc #2
    sta $20c1
.Lfbase:
.Lfpart:
.Lfall:
    ldy #10
    lda ($20d8),y
    tax
    lda #1
    sta sprite_used,x
    ldy #11
    lda ($20d8),y
    and #1
    bne .Lfnormal
    ldy #2
    sec
    lda ($20d8),y
    sbc fg_camera_slow
    sta $20c4
    iny
    lda ($20d8),y
    sbc fg_camera_slow+1
    sta $20c5
    bra .Lfposition
.Lfnormal:
    ldy #2
    sec
    lda ($20d8),y
    sbc fg_camera
    sta $20c4
    iny
    lda ($20d8),y
    sbc fg_camera+1
    sta $20c5
.Lfposition:
    beq .Lflow
    cmp #1
    bne .Lfoffscreen
    lda $20c4
    cmp #32
    bcs .Lfoffscreen
    bra .Lfvisible
.Lflow:
    lda $20c4
    cmp #17
    bcs .Lfvisible
.Lfoffscreen:
    jmp .Lfskip
.Lfvisible:
    lda sat_count
    cmp #64
    bcc 1f
    jmp .Lfdone
1:
    ldy #9
    lda ($20d8),y
    sta $20db
    dey
    lda ($20d8),y
    tax
#ifdef PCE_SGX
    /* Use the unrolled sixteen-line admission and repeated-span cache in
       exact mode; foreground pieces often share the same vertical span. */
    lda sprite_exact
    beq .Lfcheck
    stx sprite_line_lo
    lda $20db
    inc
    sta sprite_line_hi
    jsr sprite_lines_reserve
    lda sprite_line_ok
    beq .Lfskip
    bra .Lfwrite
#endif
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
#ifdef PCE_SGX
.Lfwrite:
#endif
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

/* LLVM-MOS ABI: x in A/X, y in rc2/rc3, byte result in A.
 * Cached tags imply an in-range column; cold probes retain the C bounds and
 * Arcade Card loader. Private scratch is unused by interrupts. */
.section .ram_bank129.text,"ax",@progbits
.global cell
.extern cell_slow, collision, column_tags, collision_rows
cell:
    sta $20d2
    stx $20d3
    txa
    bmi .Lcell_zero
    lda $2003
    bne .Lcell_zero
    lda $2002
    cmp collision_rows
    bcs .Lcell_zero
    lda $20d2
    and #31
    asl
    tax
    lda column_tags,x
    cmp $20d2
    bne .Lcell_cold
    lda column_tags+1,x
    cmp $20d3
    bne .Lcell_cold
    txa
    lsr
    lsr
    lsr
    lsr
    sta $20d1
    txa
    lsr
    and #7
    asl
    asl
    asl
    asl
    asl
    clc
    adc #<collision
    sta $20d0
    lda $20d1
    adc #>collision
    sta $20d1
    ldy $2002
    lda ($20d0),y
    rts
.Lcell_cold:
    lda $20d2
    ldx $20d3
    jmp cell_slow
.Lcell_zero:
    cla
    rts
