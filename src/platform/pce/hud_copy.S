/* Resident HUD retention copies, in the flow overlay with its caller.
 * At most 64 bytes per instruction so the PCM timer can service every sample. */
.section .ram_bank110.text,"ax",@progbits
.global hud_copy, hud_copy_src, hud_copy_dst, hud_copy_len, hud_copy_opcode
hud_copy:
    lda #64
    ldx hud_copy_len+1
    bne 1f
    cmp hud_copy_len
    bcc 1f
    lda hud_copy_len
1:
    sta .Lcopy+5
.Lcopy:
hud_copy_opcode:
    .byte 0x73
hud_copy_src:
    .word 0
hud_copy_dst:
    .word 0
    .word 64
    clc
    lda hud_copy_src
    adc .Lcopy+5
    sta hud_copy_src
    bcc 1f
    inc hud_copy_src+1
1:
    lda hud_copy_opcode
    cmp #0xe3
    beq .Lremaining
    clc
    lda hud_copy_dst
    adc .Lcopy+5
    sta hud_copy_dst
    bcc 1f
    inc hud_copy_dst+1
1:
.Lremaining:
    sec
    lda hud_copy_len
    sbc .Lcopy+5
    sta hud_copy_len
    bcs 1f
    dec hud_copy_len+1
1:
    ora hud_copy_len+1
    bne hud_copy
    rts
hud_copy_len:
    .word 0
