#!/usr/bin/env python3
"""The diagnostic disc's IP.BIN (make -f Makefile.saturn DIAG=1): before it jumps to the program (0.BIN, already loaded
by the BIOS), libyaul's IP turns the whole screen blue for about a second - VDP2's back screen alone, which needs no
tiles, fonts or VRAM timing - so a console that shows the blue has run our IP and loaded the program; one that never
does has not booted our disc at all.

  diag_ip.py IP.BIN      patches it in place (again on a patched one: the same bytes)

libyaul's IP (share/yaul/ip/ip.sx) ends with 'mov.l .LC1,r1; mov.l @r1,r1; jmp @r1; nop; .LC1: .long .LC0' (.LC0: the
header's 1st read address, 0x060020F0) and 256 bytes of zeros inside the IP size; .LC1 becomes the address of a word
there holding the address of the colour routine that follows it, which ends with the same jump."""
import os, struct, subprocess, sys, tempfile

IP_BASE = 0x06002000   # where the BIOS puts the IP
TAIL = bytes.fromhex('d1016112412b0009')   # mov.l .LC1,r1; mov.l @r1,r1; jmp @r1; nop
FIRST_READ = 0x060020F0

ASM = r'''
        .text
        .align 2
start:  mova    table, r0
        mov     r0, r3
        mov     #(table_end - table) / 8, r4
1:      mov.l   @r3+, r1
        mov.l   @r3+, r2
        mov.w   r2, @r1
        dt      r4
        bf      1b
        mov.l   delay, r2           ! ~1 s (4 cycles a turn at 26.8 MHz cached, slower uncached)
2:      dt      r2
        bf      2b
        mov.l   first_read, r1
        mov.l   @r1, r1
        jmp     @r1
        nop
        .align 2
delay:  .long   0x00600000
first_read: .long 0x060020F0
table:  .long   0x25F80020, 0x0000  ! BGON: no scroll screens (the BIOS's)
        .long   0x25F800E0, 0x0000  ! SDCTL: no shadow
        .long   0x25F800EC, 0x0000  ! CCCTL: no colour calculation
        .long   0x25F80110, 0x0000  ! CLOFEN: no colour offset (the BIOS's fade)
        .long   0x25F800F0, 0x0000  ! PRISA-PRISD: sprites at priority 0, hidden
        .long   0x25F800F2, 0x0000
        .long   0x25F800F4, 0x0000
        .long   0x25F800F6, 0x0000
        .long   0x25F800AC, 0x0003  ! BKTAU / BKTAL: one back screen colour, at VRAM 0x7FFFE
        .long   0x25F800AE, 0xFFFF
        .long   0x25E7FFFE, 0x7C00  ! the colour: blue
        .long   0x25F80000, 0x8000  ! TVMD: the display on, 320x224
table_end:
'''


def assemble():
    prefix = os.path.join(os.environ.get('YAUL_INSTALL_ROOT', os.path.expanduser('~/.local/x-tools/sh2eb-elf')), 'bin', 'sh2eb-elf-')
    with tempfile.TemporaryDirectory() as d:
        src, obj, raw = (os.path.join(d, n) for n in ('ip.s', 'ip.o', 'ip.raw'))
        open(src, 'w').write(ASM)
        subprocess.run([prefix + 'as', '-o', obj, src], check=True)
        subprocess.run([prefix + 'objcopy', '-O', 'binary', '-j', '.text', obj, raw], check=True)
        return open(raw, 'rb').read()


def main():
    path = sys.argv[1]
    ip = bytearray(open(path, 'rb').read())
    ip_size = struct.unpack('>I', ip[0xE0:0xE4])[0]
    t = ip.find(TAIL)
    if t < 0 or t % 4 or ip.find(TAIL, t + 1) >= 0:
        sys.exit(f'{path}: libyaul\'s jump to the program not found once')
    lit = t + 8
    code = assemble()
    ptr = lit + 8   # the word holding the routine's address, then the routine
    end = ptr + 4 + len(code)
    if end > ip_size or any(ip[lit + 4:end]) and struct.unpack('>I', ip[ptr:ptr + 4])[0] != IP_BASE + ptr + 4:
        sys.exit(f'{path}: no free room after the jump (IP size {ip_size:#x})')
    if struct.unpack('>I', ip[lit:lit + 4])[0] not in (FIRST_READ, IP_BASE + ptr):
        sys.exit(f'{path}: the jump reads {ip[lit:lit + 4].hex()}, not the 1st read address')
    ip[lit:lit + 4] = struct.pack('>I', IP_BASE + ptr)
    ip[ptr:ptr + 4] = struct.pack('>I', IP_BASE + ptr + 4)
    ip[ptr + 4:end] = code
    open(path, 'wb').write(ip)
    print(f'{path}: blue screen before the program ({len(code)} bytes at IP+{ptr + 4:#x})')


if __name__ == '__main__':
    main()
