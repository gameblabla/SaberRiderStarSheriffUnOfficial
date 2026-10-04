/* The floor samplers: one BAT row of 64 pair characters into floor_pairs.
 *
 * The ground is one byte per 8-unit cell (colour in the low nibble, class in the high one) at Arcade offset 0, 1024 cells
 * wide. A sample's position is kept as 24-bit U = u << 2 and V = v << 4 (u, v: Q3 world units, 16 bits, wrapping at 8192),
 * which puts the cell indices where the Arcade address wants them: address byte 0 = U1, byte 1 = (V1 & $fc) | U2, byte 2 = V2
 * (U2, V2 are kept masked to 2 and 4 bits). A sample is six stores and one read; a step adds the low two bytes of each axis
 * (operands patched into the loop for the row) and touches the third only on a carry or borrow (the branch / inc opcodes are
 * patched for the sign of the step). About 110 cycles a sample.
 *   floor_sample_map   128 samples: two to a pair character, high nibble first
 *   floor_sample_half  64 samples (the caller doubles du, dv): one to a pair, both nibbles alike - the far rows
 *   floor_sample_road  128 samples of the pursuit's road: only U matters, its ground is one row of 1024 cells
 * Private scratch $20c0-$20df (zero page), shared with the sprite emitter; neither runs from an IRQ. */
.section .ram_bank109.text,"ax",@progbits
.global floor_sample_map, floor_sample_half, floor_sample_road, floor_u, floor_v, floor_du, floor_dv, floor_road_hi, floor_road_b2
.extern floor_pairs
floor_u = 0x20c0
floor_v = 0x20c2
floor_du = 0x20c4
floor_dv = 0x20c6
floor_road_hi = 0x20d0      /* road sampler: Arcade address bytes 1 and 2 of the (1 KiB aligned) row, set by the caller */
floor_road_b2 = 0x20d1
DT0 = 0x20d4
DT1 = 0x20d5
DT2 = 0x20d6
U0 = 0x20c8
U1 = 0x20c9
U2 = 0x20ca
V0 = 0x20cb
V1 = 0x20cc
V2 = 0x20cd
TMP = 0x20ce

/* U = u << 2 (24 bits) from a 16-bit value, V = v << 4 */
.macro SETUP_U lo, hi, a0, a1, a2
    lda \lo
    asl
    asl
    sta \a0
    lda \lo
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    sta TMP
    lda \hi
    asl
    asl
    ora TMP
    sta \a1
    lda \hi
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    sta \a2
.endm
.macro SETUP_V lo, hi, a0, a1, a2
    lda \lo
    asl
    asl
    asl
    asl
    sta \a0
    lda \lo
    lsr
    lsr
    lsr
    lsr
    sta TMP
    lda \hi
    asl
    asl
    asl
    asl
    ora TMP
    sta \a1
    lda \hi
    lsr
    lsr
    lsr
    lsr
    sta \a2
.endm
/* Steps. `n` makes the labels unique; the immediates and opcodes are patched by PATCH_*. */
.macro STEP_U n
    clc
    lda U0
.Lsu0_\n:
    adc #0
    sta U0
    lda U1
.Lsu1_\n:
    adc #0
    sta U1
.Lsb_\n:
    bcc 1f
.Lsi_\n:
    inc U2
    lda U2
    and #3
    sta U2
1:
.endm
.macro STEP_V n
    clc
    lda V0
.Lsv0_\n:
    adc #0
    sta V0
    lda V1
.Lsv1_\n:
    adc #0
    sta V1
.Lvb_\n:
    bcc 1f
.Lvi_\n:
    inc V2
    lda V2
    and #15
    sta V2
1:
.endm
/* Patch step `n` of an axis from DT0/DT1 and the sign of the 16-bit step (bit 7 of its high byte, in A on entry's flags) */
.macro PATCH_U n, sign
    lda DT0
    sta .Lsu0_\n+1
    lda DT1
    sta .Lsu1_\n+1
    lda \sign
    bmi 3f
    lda #0x90           /* bcc */
    sta .Lsb_\n
    lda #0xe6           /* inc zp */
    sta .Lsi_\n
    bra 4f
3:  lda #0xb0           /* bcs */
    sta .Lsb_\n
    lda #0xc6           /* dec zp */
    sta .Lsi_\n
4:
.endm
.macro PATCH_V n, sign
    lda DT0
    sta .Lsv0_\n+1
    lda DT1
    sta .Lsv1_\n+1
    lda \sign
    bmi 3f
    lda #0x90
    sta .Lvb_\n
    lda #0xe6
    sta .Lvi_\n
    bra 4f
3:  lda #0xb0
    sta .Lvb_\n
    lda #0xc6
    sta .Lvi_\n
4:
.endm
.macro SAMPLE_MAP
    lda U1
    sta $1a12
    lda V1
    and #$fc
    ora U2
    sta $1a13
    lda V2
    sta $1a14
    lda $1a10
    and #15
.endm
.macro SAMPLE_ROAD
    lda U1
    sta $1a12
    lda U2
    ora floor_road_hi
    sta $1a13
    lda floor_road_b2
    sta $1a14
    lda $1a10
    and #15
.endm
/* The 24-bit start positions from floor_u / floor_v. */
.macro START_UV
    SETUP_U floor_u, floor_u+1, U0, U1, U2
    SETUP_V floor_v, floor_v+1, V0, V1, V2
.endm

floor_sample_map:
    START_UV
    SETUP_U floor_du, floor_du+1, DT0, DT1, DT2
    PATCH_U 1, floor_du+1
    PATCH_U 2, floor_du+1
    SETUP_V floor_dv, floor_dv+1, DT0, DT1, DT2
    PATCH_V 1, floor_dv+1
    PATCH_V 2, floor_dv+1
    ldx #0
.Lmap_loop:
    SAMPLE_MAP
    asl
    asl
    asl
    asl
    sta TMP
    STEP_U 1
    STEP_V 1
    SAMPLE_MAP
    ora TMP
    sta floor_pairs,x
    STEP_U 2
    STEP_V 2
    inx
    cpx #64
    beq 1f
    jmp .Lmap_loop
1:  rts

floor_sample_half:
    START_UV
    SETUP_U floor_du, floor_du+1, DT0, DT1, DT2
    PATCH_U 5, floor_du+1
    SETUP_V floor_dv, floor_dv+1, DT0, DT1, DT2
    PATCH_V 5, floor_dv+1
    ldx #0
.Lhalf_loop:
    SAMPLE_MAP
    sta TMP
    asl
    asl
    asl
    asl
    ora TMP
    sta floor_pairs,x
    STEP_U 5
    STEP_V 5
    inx
    cpx #64
    bne .Lhalf_loop
    rts

floor_sample_road:
    SETUP_U floor_u, floor_u+1, U0, U1, U2
    SETUP_U floor_du, floor_du+1, DT0, DT1, DT2
    PATCH_U 3, floor_du+1
    PATCH_U 4, floor_du+1
    ldx #0
.Lroad_loop:
    SAMPLE_ROAD
    asl
    asl
    asl
    asl
    sta TMP
    STEP_U 3
    SAMPLE_ROAD
    ora TMP
    sta floor_pairs,x
    STEP_U 4
    inx
    cpx #64
    bne .Lroad_loop
    rts
