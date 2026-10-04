/* BIOS VBlank/HBlank callbacks return with RTS, not RTI. They use no LLVM
 * imaginary registers or C stack and preserve every register they touch.
 * The stable $68 code bank remains visible during all disc/bank operations. */
.section .text.irq,"ax",@progbits
.global pce_vblank, pce_hblank
.extern pce_ticks, pce_raster_enabled, pce_floor_page, pce_raster_row
.extern pce_scroll_x, pce_scroll_y, pce_vdc_index
.extern pce_floor_pending, pce_scroll_hold
.extern pce_draws
.comm pce_visible_draw,1
.comm pce_presented,2
pce_vblank:
    pha
    inc pce_ticks
    /* Observe queued SAT generations at the hardware presentation boundary.
     * Completion can straddle an emulator frame boundary without missing
     * VBlank, so draw-loop totals alone cannot establish display cadence. */
    lda pce_draws
    cmp pce_visible_draw
    beq 1f
    sta pce_visible_draw
    inc pce_presented
    bne 1f
    inc pce_presented+1
1:
    lda pce_vdc_index
    pha
    lda pce_raster_enabled
    beq .Lnormal
    lda pce_floor_pending
    sta pce_floor_page
    st0 #7
    st1 #0
    st2 #0
    st0 #8
    st1 #128
    st2 #1
    stz pce_raster_row
    st0 #6
    st1 #184            /* the floor starts at scanline 120 (RCR = line + 64) */
    st2 #0
    bra .Ldone
.Lnormal:
    /* A platform draw holds the hardware scroll until its SAT is uploaded (video_sat_end releases
     * it), so the background and the sprites always change on the same frame. */
    lda pce_scroll_hold
    bne 1f
    st0 #7
    lda pce_scroll_x
    sta 2
    lda pce_scroll_x+1
    sta 3
1:
    st0 #8
    lda pce_scroll_y
    sta 2
    lda pce_scroll_y+1
    sta 3
    st0 #6
    st1 #255
    st2 #3
.Ldone:
    pla
    sta 0
    pla
    rts

/* 19 logical rows: the first seven repeat over 8 scanlines (from scanline 120), the last twelve over 4. The native 128x64 BAT
 * halves are selected with BXR; BYR redirects the scanline to row*8. */
pce_hblank:
    pha
    phx
    lda pce_vdc_index
    pha
    lda pce_raster_enabled
    bne .Lfloor
    st0 #7
    lda pce_scroll_x
    sta 2
    lda pce_scroll_x+1
    sta 3
    st0 #8
    lda pce_scroll_y
    clc
    adc #8
    sta 2
    lda pce_scroll_y+1
    adc #0
    sta 3
    bra .Lhlast
.Lfloor:
    st0 #7
    st1 #0
    lda pce_floor_page
    asl
    sta 3
    st0 #8
    lda pce_raster_row
    asl
    asl
    asl
    sta 2
    lda pce_raster_row
    lsr
    lsr
    lsr
    lsr
    lsr
    sta 3
    inc pce_raster_row
    lda pce_raster_row
    cmp #19
    bcs .Lhlast
    cmp #7
    bcs .Lnear
    asl
    asl
    asl
    clc
    adc #184
    bra .Lset
.Lnear:
    asl
    asl
    clc
    adc #212
.Lset:
    tax
    st0 #6
    stx 2
    lda #0
    adc #0
    sta 3
    bra .Lhdone
.Lhlast:
    st0 #6
    st1 #255
    st2 #3
.Lhdone:
    pla
    sta 0
    plx
    pla
    rts
