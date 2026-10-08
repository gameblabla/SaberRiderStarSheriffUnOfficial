#!/usr/bin/env python3
"""Ramrod's arena's four code images: app_full.elf (the whole link) holds them in virtual banks 144,129-131 (src/platform/pce/link/link.ld), where nothing is
loaded on the console. They are cut out into m6.bin (four 8 KiB banks, in the order of m6_load in video_pce.c: $76, $77, $79, $7a), and app.elf, which
the disc carries, is the link without those four banks. Higher banks remain available for resident data such as streamed PCM samples.
build_disc.py puts m6.bin into the stage 6 archive's reserved space."""
import argparse
import struct
import subprocess
from pathlib import Path

def main():
    p = argparse.ArgumentParser(); p.add_argument('--mos', type=Path, required=True); p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); out = a.out.resolve(); data = bytearray(); objcopy = str(a.mos / 'bin/llvm-objcopy')
    arena_banks=(144,129,130,131)
    for k, image in enumerate('abcd'):
        binary = out / f'm6_{image}.bin'
        subprocess.run([objcopy, '-O', 'binary', '--only-section=.ram_bank%d' % arena_banks[k], str(out / 'app_full.elf'), str(binary)], check=True)
        blob = binary.read_bytes()
        if len(blob) > 8192: raise SystemExit(f'arena image {image} is {len(blob)} bytes: over its 8 KiB bank')
        print(f'  arena image {image}: {len(blob)} of 8192 bytes', flush=True)
        data += blob + bytes(8192 - len(blob))
    (out / 'm6.bin').write_bytes(bytes(data))
    # app.elf, the link without them: their program headers become PT_NULL (pce-mkcd loads only what the headers say), the rest is the same file
    # (and their relocation sections, which pce-mkcd would patch at addresses it has no data at)
    subprocess.run([objcopy] + ['--remove-section=.rela.ram_bank%d' % b for b in arena_banks] + [str(out / 'app_full.elf'), str(out / 'app_cut.elf')], check=True)
    elf = bytearray((out / 'app_cut.elf').read_bytes())
    phoff = struct.unpack_from('<I', elf, 0x1c)[0]; phentsize, phnum = struct.unpack_from('<HH', elf, 0x2a)
    for k in range(phnum):
        at = phoff + k * phentsize
        vaddr=struct.unpack_from('<I', elf, at + 8)[0]
        if ((vaddr>>16)&255) in arena_banks: struct.pack_into('<I', elf, at, 0)
    (out / 'app.elf').write_bytes(bytes(elf))

if __name__ == '__main__': main()
