/* A single BRAM byte at low CPU speed. Only the access itself is slowed;
 * mapping/unlock/lock run at high speed. No long BIOS IRQ-masked section.
 * MPR6 is restored before IRQs reopen, so raster and PCM see normal mappings. */
.section .ram_bank114.text,"ax",@progbits
.comm bram_address,2
.comm bram_value,1
.global bram_address,bram_value,bram_read_byte,bram_write_byte
bram_read_byte:
    lda #0xad             /* LDA absolute */
    bra .Laccess
bram_write_byte:
    lda #0x8d             /* STA absolute */
.Laccess:
    php
    sei
    sta .Lbyte
    lda bram_address
    sta .Lbyte+1
    lda bram_address+1
    and #0x1f
    ora #0xc0
    sta .Lbyte+2
    tma #0x40
    pha
    lda #0xf7
    tam #0x40
    lda #0x48
    sta 0x1807
    lda #0x75
    sta 0x1807
    lda #0x80
    sta 0x1807
    lda bram_value
    csl
.Lbyte:
    lda 0xc000
    csh
    sta bram_value
    lda 0x1803            /* lock BRAM again */
    pla
    tam #0x40
    plp
    rts
