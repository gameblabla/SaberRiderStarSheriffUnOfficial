/* BIOS VBlank/HBlank callbacks return with RTS, not RTI. They use no LLVM
 * imaginary registers or C stack and preserve every register they touch.
 * The stable $68 code bank remains visible during all disc/bank operations. */
.section .text.irq,"ax",@progbits
.global pce_vblank, pce_hblank
.extern pce_ticks, pce_raster_enabled, pce_floor_page, pce_raster_row
.extern pce_road_off, pce_road_prev, columns
.extern pce_scroll_x, pce_scroll_y, pce_vdc_index
.extern pce_floor_pending, pce_scroll_hold
.extern pce_sat_pending,pce_sat_scroll_x,pce_sat_scroll_y
.extern pce_sky_far,pce_sky_near
.extern pce_race_dialog
.extern pce_draws
#ifdef PCE_SGX
.extern pce_sgx_metrics
.extern pce_metrics
.extern pce_sgx_sky_scroll_x
.extern pce_sgx_sat1_alt,pce_sgx_sat1_pending,pce_sgx_arena_hidden
.extern pce_display_on
.extern space_hull_ready
#endif
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
#ifdef PCE_SGX
.macro VDC1_INDEX r
    lda #\r
    sta 0x0010
    lda pce_sgx_metrics+4
    and #7
    ora #(\r*8)
    sta pce_sgx_metrics+4
.endm
.macro VDC1_SCROLL x,y
    VDC1_INDEX 7
    lda \x
    sta 0x0012
    lda \x+1
    sta 0x0013
    VDC1_INDEX 8
    lda \y
    sta 0x0012
    lda \y+1
    sta 0x0013
.endm

/* Called with IRQs already masked from sat_publish. Program VDC1's SATB
   pointer inside the same publication window as VDC0's pointer. */
.section .ram_bank106.text,"ax",@progbits
.global pce_sgx_sat1_commit
pce_sgx_sat1_commit:
    VDC1_INDEX 19
    stz 0x0012
    lda pce_sgx_sat1_alt
    beq .Lsgx_sat1_main
    lda #0x7e
    bra .Lsgx_sat1_address
.Lsgx_sat1_main:
    lda #0x7f
.Lsgx_sat1_address:
    sta 0x0013
    rts
.section .text.irq,"ax",@progbits
#endif
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
#ifdef PCE_SGX
    lda pce_sgx_metrics+4
    pha
#endif
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
    lda pce_race_dialog
    beq 2f
    lda #254
    sta pce_raster_row
    st1 #104            /* dialogue starts at scanline 40 */
    st2 #0
    jmp .Ldone
2:
    st1 #120            /* the near sky band (the mountains) at scanline 56, then the road from 128 */
    st2 #0
    jmp .Ldone
.Lnormal:
#ifdef PCE_SGX
    lda pce_sgx_metrics+5
    bpl .Lnormal_pce
    jmp .Lnormal_sgx
.Lnormal_pce:
    jmp .Lnormal_legacy
.Lnormal_sgx:
    jmp .Lnormal_legacy
.Lnormal_legacy:
#endif
    /* A queued generation owns its frozen scroll even if the next draw has
     * begun. Both axes stay held while an unfinished draw uploads patterns. */
    lda pce_sat_pending
    beq .Lscroll_live
    st0 #7
    lda pce_sat_scroll_x
    sta 2
    lda pce_sat_scroll_x+1
    sta 3
    st0 #8
    lda pce_sat_scroll_y
    sta 2
    lda pce_sat_scroll_y+1
    sta 3
    bra .Lscroll_done
.Lscroll_live:
    lda pce_scroll_hold
    bne .Lscroll_done
    st0 #7
    lda pce_scroll_x
    sta 2
    lda pce_scroll_x+1
    sta 3
    st0 #8
    lda pce_scroll_y
    sta 2
    lda pce_scroll_y+1
    sta 3
.Lscroll_done:
#ifdef PCE_SGX
    lda pce_sgx_metrics+5
    bmi .Lsgx_scroll_active
    jmp .Larena_vdc0
.Lsgx_scroll_active:
    /* The paired-screen flag marks scenes with a VDC0 playfield and VDC1 sky. */
    lda pce_sgx_metrics+5
    and #0x40
    beq .Lsgx_not_static_sky
    jmp .Lsgx_static_sky
.Lsgx_not_static_sky:
    lda pce_metrics+7
    cmp #7
    beq .Lsgx_stage7
    jmp .Lsgx_vdc0_zero
.Lsgx_stage7:
    lda space_hull_ready
    bne .Lsgx_hull_ready
    jmp .Lsgx_vdc1_scroll
.Lsgx_hull_ready:
    lda pce_sat_pending
    beq .Lsgx_hull_live
    st0 #7
    lda pce_sat_scroll_x
    sta 2
    lda pce_sat_scroll_x+1
    sta 3
    st0 #8
    lda pce_sat_scroll_y
    sta 2
    lda pce_sat_scroll_y+1
    sta 3
    jmp .Lsgx_vdc1_scroll
.Lsgx_hull_live:
    lda pce_scroll_hold
    beq .Lsgx_hull_scroll_live
    jmp .Lsgx_vdc1_scroll
.Lsgx_hull_scroll_live:
    st0 #7
    lda pce_scroll_x
    sta 2
    lda pce_scroll_x+1
    sta 3
    st0 #8
    lda pce_scroll_y
    sta 2
    lda pce_scroll_y+1
    sta 3
    jmp .Lsgx_vdc1_scroll
.Lsgx_static_sky:
    /* VDC0 carries the world map; VDC1 carries a static or parallax sky. */
    lda pce_sat_pending
    bne .Lsgx_static_sky_write
    lda pce_scroll_hold
    beq .Lsgx_static_sky_write
    jmp .Larena_vdc1
.Lsgx_static_sky_write:
    VDC1_INDEX 7
    lda pce_sgx_sky_scroll_x
    sta 0x0012
    lda pce_sgx_sky_scroll_x+1
    sta 0x0013
    VDC1_INDEX 8
    stz 0x0012
    stz 0x0013
    jmp .Larena_vdc1
.Lsgx_vdc0_zero:
    st0 #7
    stz 2
    stz 3
    st0 #8
    stz 2
    stz 3
    lda pce_sgx_metrics+5
    and #0x20
    beq .Lsgx_vdc1_scroll
    lda pce_sgx_metrics+5
    and #0x08
    beq .Lsgx_arena_bg_page0
    st0 #7
    stz 2
    lda #1
    sta 3
    bra .Lsgx_vdc1_scroll
.Lsgx_arena_bg_page0:
    st0 #7
    stz 2
    stz 3
.Lsgx_vdc1_scroll:
    lda pce_metrics+7
    cmp #7
    bne .Lsgx_world_scroll
    VDC1_INDEX 7
    lda pce_sky_far
    sta 0x0012
    lda pce_sky_far+1
    sta 0x0013
    VDC1_INDEX 8
    stz 0x0012
    stz 0x0013
    jmp .Larena_vdc1
.Lsgx_world_scroll:
    lda pce_sat_pending
    beq .Lsgx_world_live
    VDC1_SCROLL pce_sat_scroll_x,pce_sat_scroll_y
    jmp .Larena_vdc1
.Lsgx_world_live:
    lda pce_scroll_hold
    bne .Larena_vdc1
    VDC1_SCROLL pce_scroll_x,pce_scroll_y
    jmp .Larena_vdc1
#endif
.Larena_vdc1:
.Larena_vdc0:
    st0 #6
    st1 #255
    st2 #3
    /* Ramrod's arena (stage 6): the floor, from scanline 128, in 24 groups of four scanlines (the picture's tables, arena_floor in console RAM - the staging banks are not always mapped -: BXR and BYR a group) */
    lda pce_arena_raster
    beq .Ldone
    stz pce_raster_row
#ifdef PCE_SGX
    lda pce_sgx_metrics+5
    bmi .Larena_vdc1_start
#endif
    st0 #6
    st1 #192
    st2 #0
    jmp .Ldone
#ifdef PCE_SGX
.Larena_vdc1_start:
    VDC1_INDEX 6
    lda #192
    sta 0x0012
    stz 0x0013
#endif
.Ldone:
    stz pce_sat_pending
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
#ifdef PCE_SGX
    lda pce_sgx_sat1_pending
    beq .Lsgx_sat1_done
    stz pce_sgx_sat1_pending
    lda pce_sgx_arena_hidden
    beq .Lsgx_sat1_done
    stz pce_sgx_arena_hidden
    lda pce_display_on
    beq .Lsgx_sat1_done
    VDC1_INDEX 5
    lda pce_sgx_metrics+5
    and #0x10
    beq .Lsgx_sat1_sprite_only
    lda #0xc0
    bra .Lsgx_sat1_control
.Lsgx_sat1_sprite_only:
    lda #0x40
.Lsgx_sat1_control:
    sta 0x0012
    stz 0x0013
.Lsgx_sat1_done:
    pla
    sta pce_sgx_metrics+4
    lsr
    lsr
    lsr
    sta 0x0010
#endif
    pla
    sta 0
    pla
    rts

/* The race's raster, from the end of scanline 79: the near sky band, then one interrupt for each pair of scanlines from the end of
 * scanline 127 to 221 (the sky and its mountains fill scanlines 0-127: the road's far rows are fully hazed down to there; the table
 * entries m < 16 are not used). Interrupt p (pce_raster_row, 8 to 55) is at the end of scanline 111 + 2p and sets what scanlines 112 + 2p and 113 + 2p
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
#ifdef PCE_SGX
    lda pce_sgx_metrics+4
    pha
#endif
    lda pce_raster_enabled
    beq 4f
    jmp .Lfloor
4:
#ifdef PCE_SGX
    lda pce_sgx_metrics+5
    bmi .Lsgx_hblank
#endif
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
    jmp .Lhlast
#ifdef PCE_SGX
.Lsgx_hblank:
    lda pce_arena_raster
    bne 5f
    jmp .Lsgx_scroll_line
5:
    lda pce_raster_row
    cmp #24
    bcc 6f
    jmp .Lhlast
6:
    asl
    asl
    tax
    VDC1_INDEX 7
    lda arena_floor,x
    sta 0x0012
    lda arena_floor+1,x
    sta 0x0013
    VDC1_INDEX 8
    lda arena_floor+2,x
    sta 0x0012
    lda arena_floor+3,x
    sta 0x0013
    VDC1_INDEX 6
    lda pce_raster_row
    asl
    asl
    clc
    adc #128
    sta 0x0012
    stz 0x0013
    inc pce_raster_row
    jmp .Lhlast
.Lsgx_scroll_line:
    lda pce_sgx_metrics+5
    and #0x40
    beq .Lsgx_scroll_world
    jmp .Lhlast
.Lsgx_scroll_world:
    lda pce_metrics+7
    cmp #7
    bne .Lsgx_world_line
    VDC1_INDEX 7
    lda pce_sky_far
    sta 0x0012
    lda pce_sky_far+1
    sta 0x0013
    VDC1_INDEX 8
    stz 0x0012
    stz 0x0013
    lda space_hull_ready
    bne 7f
    st0 #7
    lda pce_sky_near
    sta 2
    lda pce_sky_near+1
    sta 3
    jmp .Lhlast
7:
    st0 #7
    lda pce_scroll_x
    sta 2
    lda pce_scroll_x+1
    sta 3
    st0 #8
    lda pce_scroll_y
    sta 2
    lda pce_scroll_y+1
    sta 3
    jmp .Lhlast
.Lsgx_world_line:
    lda pce_sat_pending
    beq .Lsgx_world_live_line
    VDC1_SCROLL pce_sat_scroll_x,pce_sat_scroll_y
    bra .Lsgx_zero_vdc0
.Lsgx_world_live_line:
    lda pce_scroll_hold
    bne .Lsgx_zero_vdc0
    VDC1_SCROLL pce_scroll_x,pce_scroll_y
.Lsgx_zero_vdc0:
    st0 #7
    stz 2
    stz 3
    st0 #8
    stz 2
    stz 3
    jmp .Lhlast
#endif
.Lfloor:
    lda pce_raster_row
    cmp #254
    bne 2f
    st0 #7
    st1 #0
    st2 #0
    dec pce_raster_row
    st0 #6
    st1 #152            /* restore the frozen sky below the box, scanline 88 */
    st2 #0
    jmp .Lhdone
2:
    cmp #253
    beq .Lnear
    cmp #255
    bne .Lroad
.Lnear:
    st0 #7
    lda pce_sky_near
    sta 2
    lda pce_sky_near+1
    sta 3
    lda #8
    sta pce_raster_row
    st0 #6
    st1 #192
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
#ifdef PCE_SGX
    lda pce_sgx_metrics+5
    bmi .Larena_sgx_row
#endif
    inc pce_raster_row
    lda pce_raster_row
    cmp #24
    bcc 8f
    jmp .Lhlast
8:
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
    jmp .Lhdone
#ifdef PCE_SGX
.Larena_sgx_row:
    VDC1_INDEX 7
    lda arena_floor,x
    sta 0x0012
    lda arena_floor+1,x
    sta 0x0013
    VDC1_INDEX 8
    lda arena_floor+2,x
    sta 0x0012
    lda arena_floor+3,x
    sta 0x0013
    inc pce_raster_row
    lda pce_raster_row
    cmp #24
    bcc 9f
    jmp .Lhlast
9:
    asl
    asl
    clc
    adc #128
    sta 0x0012
    stz 0x0013
#endif
.Lhdone:
#ifdef PCE_SGX
    pla
    sta pce_sgx_metrics+4
    lsr
    lsr
    lsr
    sta 0x0010
#endif
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
