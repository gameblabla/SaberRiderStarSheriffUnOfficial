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
.comm pce_arena_raster,1
.global pce_arena_raster
.comm arena_floor,96
.global arena_floor
.comm vce_q_head,1
.comm vce_q_tail,1
.comm vce_q_idx,4
.comm vce_q_data,128
.global vce_q_head, vce_q_tail, vce_q_idx, vce_q_data
/* One queued palette: two TIA block moves of 16 bytes into the VCE data port (alternating $0404/$0405). */
.macro VQ_COPY k
    .byte 0xe3
    .word vce_q_data+\k*32
    .word 0x0404
    .word 16
    .byte 0xe3
    .word vce_q_data+\k*32+16
    .word 0x0404
    .word 16
.endm
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
    /* Ramrod's arena (stage 6): the floor, from scanline 128, in 24 groups of four scanlines (the picture's tables, arena_floor in console RAM - the staging banks are not always mapped -: BXR and BYR a group) */
    lda pce_arena_raster
    beq .Ldone
    stz pce_raster_row
    st0 #6
    st1 #192
    st2 #0
.Ldone:
    /* The palettes queued for the VCE (vce_pce.c): written now, in the vertical blank, 16 bytes a block move. */
    phx
    ldx vce_q_head
    cpx vce_q_tail
    beq .Lvq_out
.Lvq_loop:
    lda vce_q_idx,x
    pha
    asl
    asl
    asl
    asl
    sta 0x0402
    pla
    lsr
    lsr
    lsr
    lsr
    sta 0x0403
    txa
    beq .Lvq_s0
    cmp #1
    beq .Lvq_s1
    cmp #2
    beq .Lvq_s2
    VQ_COPY 3
    bra .Lvq_next
.Lvq_s0:
    VQ_COPY 0
    bra .Lvq_next
.Lvq_s1:
    VQ_COPY 1
    bra .Lvq_next
.Lvq_s2:
    VQ_COPY 2
.Lvq_next:
    inx
    txa
    and #3
    tax
    cpx vce_q_tail
    bne .Lvq_loop
    stx vce_q_head
.Lvq_out:
    plx
    pla
    sta 0
    pla
    rts

/* The race's raster, from the end of scanline 79: the near sky band, then one interrupt for each pair of scanlines from the end of
 * scanline 111 to 221. Interrupt p (pce_raster_row) is at the end of scanline 111 + 2p and sets what scanlines 112 + 2p and 113 + 2p
 * show: BXR from the tables road_pce.c built (low byte at columns + 0, high at +256, both indexed by m = 2p plus the half on display)
 * and, where the picture copy changes (the stripes, or a window that wraps round the picture), BYR. The picture is two copies of 12 BAT
 * rows (rows 0-11, 12-23; scanline 128 reads row 0 by itself since BYR counts on from the sky's 384) after the haze rows 62-63 at
 * scanlines 112-127, and four copies of the lower six rows (24-29 and 30-35 when the window wraps past the right edge, 36-41 and
 * 42-47 past the left, each in the two stripe phases). The third table holds the copy's class 0-5 (A, B, then those four); BYR is the
 * line m plus the class's constant (pce_byr_lo/hi: 16 bits). A BYR written
 * in the handler shows one line later (the VDC counts it once before the next scanline), hence the minus one. */
pce_hblank:
    pha
    phx
    lda pce_vdc_index
    pha
    lda pce_raster_enabled
    bne .Lfloor
    lda pce_arena_raster
    beq 1f
    jmp .Larena
1:
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
    jmp .Lhdone
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
    clc
    adc pce_byr_lo,x
    st0 #8
    sta 2
    cla
    adc pce_byr_hi,x
    sta 3
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
    jmp .Lhdone
.Lhlast:
    st0 #6
    st1 #255
    st2 #3
    jmp .Lhdone
.Larena:
    lda pce_raster_row
    asl
    asl
    tax
    st0 #7
    lda arena_floor+0,x
    sta 2
    lda arena_floor+1,x
    sta 3
    st0 #8
    lda arena_floor+2,x
    sta 2
    lda arena_floor+3,x
    sta 3
    inc pce_raster_row
    lda pce_raster_row
    cmp #24
    bcs .Lhlast
    asl
    asl
    clc
    adc #192
    tax
    st0 #6
    stx 2
    lda #0
    adc #0
    sta 3
.Lhdone:
    pla
    sta 0
    plx
    pla
    rts
/* BYR of the first line of a pair minus m, for each copy class: A -17, B +79, then BAT rows 24, 30, 36 and 42 for image row 8 (line 64):
 * 8 * row - 65. (The minus one is the VDC's count of a BYR written in the handler.) */
pce_byr_lo:
    .byte 239,79,127,175,223,15
pce_byr_hi:
    .byte 255,0,0,0,0,1
