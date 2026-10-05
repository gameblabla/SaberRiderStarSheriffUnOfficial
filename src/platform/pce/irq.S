/* BIOS VBlank/HBlank callbacks return with RTS, not RTI. They use no LLVM
 * imaginary registers or C stack and preserve every register they touch.
 * The stable $68 code bank remains visible during all disc/bank operations. */
.section .text.irq,"ax",@progbits
.global pce_vblank, pce_hblank
.extern pce_ticks, pce_raster_enabled, pce_floor_page, pce_raster_row
.extern pce_road_off, pce_road_prev, columns
.extern pce_scroll_x, pce_scroll_y, pce_vdc_index
.extern pce_floor_pending, pce_scroll_hold
.extern pce_sky_far,pce_sky_near
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
    lsr                 /* the road's tables: the half the latched page names (offset 0 or 128) */
    lda #0
    ror
    sta pce_road_off
    stz pce_road_prev
    st0 #7
    lda pce_sky_far
    sta 2
    lda pce_sky_far+1
    sta 3
    st0 #8
    st1 #128
    st2 #1
    lda #255
    sta pce_raster_row
    st0 #6
    st1 #144            /* sky band at scanline 80, then the road from 112 */
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

/* The race's raster, from the end of scanline 79: the near sky band, then one interrupt for each pair of scanlines from the end of
 * scanline 111 to 221. Interrupt p (pce_raster_row) is at the end of scanline 111 + 2p and sets what scanlines 112 + 2p and 113 + 2p
 * show: BXR from the tables road_pce.c built (low byte at columns + 0, high at +256, both indexed by m = 2p plus the half on display)
 * and, where the picture copy changes (the stripes), BYR. The picture is two copies of 12 BAT rows (rows 0-11, 12-23; scanline 128
 * reads row 0 by itself since BYR counts on from the sky's 384) after the haze rows 62-63 at scanlines 112-127. A BYR written
 * in the handler shows one line later (the VDC counts it once before the next scanline), hence the minus one. */
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
    lda pce_raster_row
    cmp #255
    bne .Lroad
    st0 #7
    lda pce_sky_near
    sta 2
    lda pce_sky_near+1
    sta 3
    stz pce_raster_row
    st0 #6
    st1 #176
    st2 #0
    bra .Lhdone
.Lroad:
    asl
    clc
    adc pce_road_off
    tax
    st0 #7
    lda columns,x
    sta 2
    lda columns+256,x
    sta 3
    lda columns+512,x
    cmp pce_road_prev
    beq .Lsame
    sta pce_road_prev
    tax
    lda pce_raster_row
    asl
    cpx #0
    bne .Lcopyb
    sec
    sbc #17             /* copy A: scanline m + 112 reads image line m + 112 - 128 */
    bra .Lbyr
.Lcopyb:
    clc
    adc #79             /* copy B: BAT row 12 holds image line 128 */
.Lbyr:
    st0 #8
    sta 2
    st2 #0
.Lsame:
    inc pce_raster_row
    lda pce_raster_row
    cmp #56
    bcs .Lhlast
    asl
    clc
    adc #176
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
