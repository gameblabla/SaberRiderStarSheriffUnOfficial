; ------------------------------------------------------
; LZ40 Decompression (SH-4 Assembly)
; Author: VincentNL (2024)
; ------------------------------------------------------
; Vendored for reference from:
;   https://github.com/VincentNLOBJ/LZ40_Decompress_SH4
;   MIT License (c) 2024 VincentNL (see LICENSE.VincentNLOBJ).
; Credits: Original LZX (LZ40) format by CUE
;   Reference: https://www.romhacking.net/utilities/826/
; ------------------------------------------------------
; Saber Rider note: this ASM is the reference for the Dreamcast FMV/pack
; LZ40 format. The build uses the portable C port in lz40.h (same byte
; format, no extra tables/mallocs); this file is not assembled.
; ------------------------------------------------------

; ------------------------------------------------------

; Purpose:
; Decompress LZ40 compressed data.
; SH-4 architecture (SEGA Dreamcast / Naomi hardware).

; Registers Used:
; ------------------------------------------------------
; r0: temp             ; General-purpose register
; r1: temp             ; General-purpose register
; r2: length           ; Copy length for data references
; r3: mask             ; Bit mask for flag processing
; r4: out_ptr          ; Pointer to decompressed output
; r5: input_ptr        ; Pointer to compressed input data
; r6: out_eof_ptr      ; End of output data buffer
; r7: flags            ; Current flags byte
; ------------------------------------------------------


; Please note, you just need to pass r4 and r5 into this function.

LZ40_decompress:
          add         0x1,r5          ; Skip header bytes, MAGIC byte (\x40)
          mov.b       @r5+,r6         ; Read first byte of decompressed length
          mov.b       @r5+,r0         ; Read next byte
          extu.b      r6,r6           ; Zero-extend byte to 32-bit
          extu.b      r0,r0           ; Zero-extend byte to 32-bit
          shll8       r0              ; Shift r0 to combine it with r6
          or          r0,r6
          mov.b       @r5+,r0         ; Read the third byte of decompressed length
          extu.b      r0,r0
          shll16      r0              ; Shift left by 16 bits
          or          r0,r6           ; Combine all length bytes into r6
          mov.l       r6,@-r15        ; Save decompressed length to stack
          add         r4,r6           ; Set r6 as end of decompressed data pointer
          mov         0,r3            ; Initialize mask to 0

loc_main_loop:
          cmp/hs      r6, r4           ; Check if decompression is complete
          bt          loc_END          ; Exit if r4 >= r6 (end of file)

          ; Refill mask if empty
          tst         r3, r3           ; Test if mask is 0
          bf          loc_check_flag
          mov.b       @r5+, r7         ; Load new flags byte
          extu.b      r7, r7           ; Zero-extend flags byte
          neg         r7, r7           ; Invert flags for bit-checking
          mov         0x80,r3          ; Set mask to 0x80
          extu.b      r3,r3            ; Zero-extend mask

loc_check_flag:
          mov         r3, r0           ; r0 = mask
          and         r7, r0           ; Check the current flag (flags & mask)
          tst         r0, r0
          bf          loc_back_refs    ; Branch if flag is 0 (data reference)

          ; Literal byte copy
          mov.b       @r5+, r1         ; Load literal byte
          mov.b       r1, @r4          ; Write to output buffer
          bra         loc_next_flag
          add         1, r4            ; Increment output pointer

loc_back_refs:
          ; Load reference offset (position)
          mov.b       @r5+, r1
          extu.b      r1, r1
          mov.b       @r5+, r0
          extu.b      r0, r0
          shll8       r0
          or          r0, r1           ; Combine bytes into r1 (position)
          mov         r1, r2           ; Copy position into r2
          mov         r2, r0
          and         0xF, r0          ; Extract copy length from low nibble
          mov         r0, r2           ; Save copy length

loc_lenght_test:
          mov         2,r0             ; THRESHOLD: Minimum copy length
          cmp/hs      r0, r2           ; Compare copy length with threshold
          bt          loc_copy_data    ; If copy_len < 2, copy data

; Copy length > 2
          mov.b       @r5+, r2         ; Load extended copy length
          extu.b      r2, r2           ; Zero-extend copy length
          mov         r1, r0           ; Reload position into r0
          and         0xF, r0          ; Extract low nibble
          tst         r0, r0           ; Check if nibble is 0
          bf          loc_long_copy

loc_short_copy:
; Copy length <= 2
          bra         loc_copy_data
          add         0x10,r2          ; Add 0x10 to copy length if nibble is 0

loc_long_copy:

          mov.b       @r5+, r0         ; Load extended copy length (high byte)
          extu.b      r0, r0
          shll8       r0
          or          r0, r2           ; Combine into r2 (copy length)
          mov         0x44,r0
          shll2       r0
          add         r0,r2            ; Add 0x110 to copy length


loc_copy_data:
          ; Copy data from reference
          tst         r2, r2           ; Check if copy length is 0
          bt          loc_next_flag

          shlr2       r1               ; Shift right by 2 bits (first step)
          shlr2       r1               ; Shift right by another 2 bits (total 4 bits)

loc_copy_loop:
          mov         r4, r0           ; Load current output pointer
          sub         r1, r0           ; Calculate reference position
          mov.b       @r0, r0          ; Load byte from reference
          mov.b       r0, @r4          ; Write byte to output
          add         1, r4            ; Increment output pointer
          dt          r2               ; Decrement copy length
          bf          loc_copy_loop    ; Repeat until copy length is 0


loc_next_flag:

          bra         loc_main_loop
          shlr        r3                ; Shift mask right

loc_END:
          rts                           ; Return from subroutine
          mov.l       @r15+, r0         ; Restore value from stack
