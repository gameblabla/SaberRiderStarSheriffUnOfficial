/* SAT upload shares the short memory-transfer loop with HUD retention.
 * Setup is atomic; the PCM timer may run between every pair of bursts. */
.section .ram_bank110.text,"ax",@progbits
.global sat_copy, sat_copy_word, sat_copy_src, sat_copy_len
.extern hud_copy, hud_copy_src, hud_copy_dst, hud_copy_len, hud_copy_opcode, pce_vdc_index
.comm sat_copy_word,2
.comm sat_copy_src,2
.comm sat_copy_len,2
sat_copy:
    lda sat_copy_src
    sta hud_copy_src
    lda sat_copy_src+1
    sta hud_copy_src+1
    lda sat_copy_len
    sta hud_copy_len
    lda sat_copy_len+1
    sta hud_copy_len+1
    lda #2
    sta hud_copy_dst
    stz hud_copy_dst+1
    lda #$e3
    sta hud_copy_opcode
    php
    sei
    st0 #0
    lda sat_copy_word
    sta $0002
    lda sat_copy_word+1
    sta $0003
    lda #2
    sta pce_vdc_index
    sta $20f7
    st0 #2
    plp
    jmp hud_copy
