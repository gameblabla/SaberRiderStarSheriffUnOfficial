/* Emit the admitted columns in the original top/bottom SAT order. The C
 * caller has checked clipping, scanline capacity and SAT space. Private ZP
 * is shared with sprite_emit, which has returned before the herd is drawn. */
.section .ram_bank111.text,"ax",@progbits
.global herd_emit
.extern herd_emit_args, sat, sat_count, sat_page
#ifdef PCE_SGX
.extern pce_sgx_metrics
#endif
.macro cell count, pattern, row, attribute
    lda herd_emit_args+\count
    beq .Lskip\@
    dec herd_emit_args+\count
    ldy #0
    clc
    lda herd_emit_args+2
    adc #\row
    sta ($20c0),y
    iny
    lda herd_emit_args+3
    adc #0
    sta ($20c0),y
    iny
    lda herd_emit_args
    sta ($20c0),y
    iny
    lda herd_emit_args+1
    sta ($20c0),y
    iny
    lda herd_emit_args+\pattern
    sta ($20c0),y
    iny
    lda herd_emit_args+\pattern+1
    sta ($20c0),y
    iny
    lda #$8f
#ifdef PCE_SGX
    ldx pce_sgx_metrics+5
    bpl 2f
    lda #$8d
2:
#endif
    sta ($20c0),y
    iny
    lda #\attribute
    sta ($20c0),y
    clc
    lda $20c0
    adc #8
    sta $20c0
    bcc 1f
    inc $20c1
1:
    inc sat_count
.Lskip\@:
.endm
herd_emit:
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
    asl
    clc
    adc $20c1
    sta $20c1
.Lcolumn:
    cell 8, 4, 0, $31
    cell 9, 6, 64, $01
    clc
    lda herd_emit_args
    adc #32
    sta herd_emit_args
    bcc 1f
    inc herd_emit_args+1
1:
    clc
    lda herd_emit_args+4
    adc #16
    sta herd_emit_args+4
    bcc 1f
    inc herd_emit_args+5
1:
    clc
    lda herd_emit_args+6
    adc #4
    sta herd_emit_args+6
    bcc 1f
    inc herd_emit_args+7
1:
    dec herd_emit_args+10
    beq 1f
    jmp .Lcolumn
1:
    rts
