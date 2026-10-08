/* SAT upload shares the short memory-transfer loop with HUD retention.
 * Setup is atomic; the PCM timer may run between every pair of bursts. */
.section .ram_bank114.text,"ax",@progbits
.global sat_copy, sat_copy_word, sat_copy_src, sat_copy_len, sat_copy_vdc
.extern hud_copy, hud_copy_src, hud_copy_dst, hud_copy_len, hud_copy_opcode, pce_vdc_index
#ifdef PCE_SGX
.extern pce_sgx_metrics
.macro VDC1_INDEX r
    lda #\r
    sta 0x0010
    lda pce_sgx_metrics+4
    and #7
    ora #(\r*8)
    sta pce_sgx_metrics+4
.endm
#endif
.comm sat_copy_word,2
.comm sat_copy_src,2
.comm sat_copy_len,2
.pushsection .ram_bank107.work,"aw",@nobits
.global sat_copy_vdc
sat_copy_vdc:
.space 1
.popsection
sat_copy:
    lda sat_copy_src
    sta hud_copy_src
    lda sat_copy_src+1
    sta hud_copy_src+1
    lda sat_copy_len
    sta hud_copy_len
    lda sat_copy_len+1
    sta hud_copy_len+1
    lda #$e3
    sta hud_copy_opcode
    php
    sei
#ifdef PCE_SGX
    lda sat_copy_vdc
    beq .Lsat_vdc0
    VDC1_INDEX 0
    lda sat_copy_word
    sta 0x0012
    lda sat_copy_word+1
    sta 0x0013
    VDC1_INDEX 2
    lda #$12
    sta hud_copy_dst
    stz hud_copy_dst+1
    plp
    jmp hud_copy
.Lsat_vdc0:
#endif
    st0 #0
    lda sat_copy_word
    sta $0002
    lda sat_copy_word+1
    sta $0003
    lda #2
    sta pce_vdc_index
    sta $20f7
    st0 #2
    lda #2
    sta hud_copy_dst
    stz hud_copy_dst+1
    plp
    jmp hud_copy
