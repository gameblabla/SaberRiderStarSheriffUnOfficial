/*
MIT License

Copyright (c) 2020 Daniel Serpell

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/* ZX02 decoder adapted from DMSC's MIT-licensed zx02-optim.asm (2022).
 * Source: ../PCE/zx02/6502/zx02-optim.asm; license: ../PCE/zx02/LICENSE.
 * Input: compressed block in ADPCM RAM, beginning at zx02_source.
 * Output: Arcade port 0, already positioned. Port 1 reads backreferences.
 * Private scratch $d2-$d4; audio is stopped and raster IRQs masked by loader.
 * No CPU input/output buffer or changes to mapped code banks.
 */
#ifdef PCE_SGX
.section .ram_bank130.text,"ax",@progbits
.global zx02_arcade
zx02_arcade:
    lda zx02_source
    sta $1808
    lda zx02_source+1
    sta $1809
    lda #8
    tsb $180d
    lda $180a
    ldx #4
1:
    dex
    bne 1b
    lda #8
    trb $180d
    jsr .Lbyte                /* discard the read-pointer pipeline byte */
    stz $20d2                 /* offset minus one */
    stz $20d3
    lda #$80
    sta $20d4                 /* bit reservoir */
    cly
.Lliteral:
    ldx #1
    jsr .Lelias
.Lliteral_byte:
    jsr .Lbyte
    sta $1a00
    dex
    bne .Lliteral_byte
    asl $20d4
    bcs .Loffset
    ldx #1
    jsr .Lelias
.Lmatch:
    /* base registers expose the write cursor after each auto-increment. */
    /* Subtract offset+1 using the clear carry's implicit borrow. */
    clc
    lda $1a02
    sbc $20d2
    sta $1a12
    lda $1a03
    sbc $20d3
    sta $1a13
    lda $1a04
    sbc #0
    sta $1a14
    stz $1a15
    stz $1a16
    lda #1
    sta $1a17
    stz $1a18
    lda #$11
    sta $1a19
.Lmatch_byte:
    lda $1a10
    sta $1a00
    dex
    bne .Lmatch_byte
    asl $20d4
    bcc .Lliteral
.Loffset:
    ldx #1
    jsr .Lelias
    cpx #0
    beq .Lend
    dex
    txa
    lsr
    sta $20d3
    jsr .Lbyte
    ror
    sta $20d2
    ldx #1
    jsr .Lelias_skip
    inx
    bra .Lmatch
.Lelias_data:
    asl $20d4
    txa
    rol
    tax
.Lelias:
    asl $20d4
    bne .Lelias_skip
    jsr .Lbyte
    rol
    sta $20d4
.Lelias_skip:
    bcs .Lelias_data
.Lend:
    rts
.Lbyte:
    /* Read-busy is bit 7. BIT and LDA preserve Elias/offset carry. */
    bit $180c
    bmi .Lbyte
    lda $180a
    rts
#endif
