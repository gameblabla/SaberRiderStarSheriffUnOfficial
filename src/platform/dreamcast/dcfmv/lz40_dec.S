! ------------------------------------------------------
! LZ40 Decompression (SH-4 Assembly, GCC GAS port)
! Author of the original: VincentNL (2024)
!   https://github.com/VincentNLOBJ/LZ40_Decompress_SH4
!   MIT License (c) 2024 VincentNL (see LICENSE.VincentNLOBJ).
! Credits: Original LZX (LZ40) format by CUE
!   Reference: https://www.romhacking.net/utilities/826/
! ------------------------------------------------------
! GAS port of the vendored LZ40_dec.asm for sh-elf-gcc / kos-cc.
! Logic is verbatim; only the syntax was adapted:
!   - `;' comments -> `!' (GAS uses `;' as a statement separator)
!   - immediates take `#': mov #0,r3 / add #1,r4 / and #0xF,r0
!   - global symbol carries the ELF underscore prefix: _LZ40_decompress
!   - local labels (.L...) so nothing leaks into the symbol table
! ------------------------------------------------------
! Purpose:
! Decompress LZ40 compressed data.
! SH-4 architecture (SEGA Dreamcast / Naomi hardware).
!
! ABI (SH ELF, leaf function: PR is preserved, only r0-r7 touched):
!   r4: out_ptr   (uint8_t *dst)
!   r5: input_ptr (const uint8_t *src, points at the 0x40 magic)
!   returns the decompressed length in r0 (the 24-bit header length).
! Registers used:
!   r0: temp
!   r1: temp
!   r2: length (copy length for data references)
!   r3: mask (bit mask for flag processing)
!   r4: out_ptr (pointer to decompressed output, advances)
!   r5: input_ptr (pointer to compressed input data, advances)
!   r6: out_eof_ptr (end of output data buffer)
!   r7: flags (current flags byte, negated on refill)
! ------------------------------------------------------
! NOTE: like the original, the fast loop trusts the stream (no input
! bound or offset checks inside). The C wrapper in lz40.h validates the
! 0x40 header and declen <= dstcap before calling here; corrupt streams
! beyond that are undefined, same as upstream.
! ------------------------------------------------------

	.text
	.align 2
	.global _LZ40_decompress
	.type _LZ40_decompress,@function

_LZ40_decompress:
	add	#1,r5			! Skip header magic byte (0x40)
	mov.b	@r5+,r6			! Read first byte of decompressed length
	mov.b	@r5+,r0			! Read next byte
	extu.b	r6,r6			! Zero-extend byte to 32-bit
	extu.b	r0,r0			! Zero-extend byte to 32-bit
	shll8	r0			! Shift r0 to combine it with r6
	or	r0,r6
	mov.b	@r5+,r0			! Read the third byte of decompressed length
	extu.b	r0,r0
	shll16	r0			! Shift left by 16 bits
	or	r0,r6			! Combine all length bytes into r6
	mov.l	r6,@-r15		! Save decompressed length to stack
	add	r4,r6			! Set r6 as end of decompressed data pointer
	mov	#0,r3			! Initialize mask to 0

.Llz40_main_loop:
	cmp/hs	r6,r4			! Check if decompression is complete (r4 >= r6?)
	bt	.Llz40_end		! Exit if r4 >= r6 (end of file)

	! Refill mask if empty
	tst	r3,r3			! Test if mask is 0
	bf	.Llz40_check_flag
	mov.b	@r5+,r7			! Load new flags byte
	extu.b	r7,r7			! Zero-extend flags byte
	neg	r7,r7			! Negate flags for bit-checking (-stored)
	mov	#0x80,r3		! Set mask to 0x80 (encodes as -128, extu.b -> 0x80)
	extu.b	r3,r3			! Zero-extend mask

.Llz40_check_flag:
	mov	r3,r0			! r0 = mask
	and	r7,r0			! Check the current flag (flags & mask)
	tst	r0,r0
	bf	.Llz40_back_refs	! Branch if flag != 0 (data reference)

	! Literal byte copy
	mov.b	@r5+,r1			! Load literal byte
	mov.b	r1,@r4			! Write to output buffer
	bra	.Llz40_next_flag
	add	#1,r4			! Increment output pointer (delay slot)

.Llz40_back_refs:
	! Load reference offset (position)
	mov.b	@r5+,r1
	extu.b	r1,r1
	mov.b	@r5+,r0
	extu.b	r0,r0
	shll8	r0
	or	r0,r1			! Combine bytes into r1 (position)
	mov	r1,r2			! Copy position into r2
	mov	r2,r0
	and	#0xF,r0			! Extract copy length from low nibble
	mov	r0,r2			! Save copy length

.Llz40_length_test:
	mov	#2,r0			! THRESHOLD: Minimum copy length
	cmp/hs	r0,r2			! Compare copy length with threshold (r2 >= 2?)
	bt	.Llz40_copy_data	! If copy_len >= 2, short copy

! Extended copy length
	mov.b	@r5+,r2			! Load extended copy length
	extu.b	r2,r2			! Zero-extend copy length
	mov	r1,r0			! Reload position into r0
	and	#0xF,r0			! Extract low nibble
	tst	r0,r0			! Check if nibble is 0
	bf	.Llz40_long_copy

.Llz40_short_copy:
! Copy length nibble was 0
	bra	.Llz40_copy_data
	add	#0x10,r2		! Add 0x10 to copy length if nibble is 0 (delay slot)

.Llz40_long_copy:
	mov.b	@r5+,r0			! Load extended copy length (high byte)
	extu.b	r0,r0
	shll8	r0
	or	r0,r2			! Combine into r2 (copy length)
	mov	#0x44,r0
	shll2	r0
	add	r0,r2			! Add 0x110 to copy length


.Llz40_copy_data:
	! Copy data from reference
	tst	r2,r2			! Check if copy length is 0
	bt	.Llz40_next_flag

	shlr2	r1			! Shift right by 2 bits (first step)
	shlr2	r1			! Shift right by another 2 bits (total 4 bits)

.Llz40_copy_loop:
	mov	r4,r0			! Load current output pointer
	sub	r1,r0			! Calculate reference position
	mov.b	@r0,r0			! Load byte from reference
	mov.b	r0,@r4			! Write byte to output
	add	#1,r4			! Increment output pointer
	dt	r2			! Decrement copy length (T=1 when 0)
	bf	.Llz40_copy_loop	! Repeat until copy length is 0


.Llz40_next_flag:

	bra	.Llz40_main_loop
	shlr	r3			! Shift mask right (delay slot)

.Llz40_end:
	rts				! Return from subroutine
	mov.l	@r15+,r0		! Restore decompressed length into r0 (delay slot)

	.size _LZ40_decompress,.-_LZ40_decompress
