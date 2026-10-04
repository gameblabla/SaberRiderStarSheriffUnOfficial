/* Update display-generation pins and LRU stamps without C array arithmetic.
 * Called by sat_begin_body in the same flow overlay. IRQs use no scratch. */
.section .ram_bank110.text,"ax",@progbits
.global sprite_cache_begin
.extern sprite_used, sprite_pinned, sprite_stamp, sprite_epoch
sprite_cache_begin:
    ldx #47
.Lslot:
    lda sprite_used,x
    beq .Lunused
    lda #2
    sta sprite_pinned,x
    lda sprite_epoch
    sta sprite_stamp,x
    bra .Lclear
.Lunused:
    lda sprite_pinned,x
    beq .Lclear
    dec sprite_pinned,x
.Lclear:
    stz sprite_used,x
    dex
    bpl .Lslot
    rts
