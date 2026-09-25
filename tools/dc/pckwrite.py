"""Write E2DM "HEADLIST" packs (the format src/pack.c reads) for the Dreamcast disc.

Every block starts on a 32-byte boundary and is a multiple of 32 bytes long, so the game reads one straight into a
32-byte aligned buffer in a single read and can DMA it on to video or sound memory. Blocks are stored raw (no LZO), or
LZ40-compressed where asked (directory type "<type>+lz40"; the Dreamcast decodes those with
src/platform/dreamcast/dcfmv/lz40.h, the C port of VincentNLOBJ's SH-4 LZ40 decoder). The packer lays
blocks out in the order they are added: put the ones a stage loads together next to each other.

Layout: "HEADLIST", directory offset, directory length (unpacked); 16-byte entries from 0x10 (8 hex digits of id,
offset, size) ended by a zero entry; the LZO1Z-coded "type=ID\\n" directory; the blocks.
"""
from __future__ import annotations

import struct

from lz40 import lz40_compress


def pad32(data: bytes) -> bytes:
    return data + b'\0' * (-len(data) % 32)


def lzo_literals(data: bytes) -> bytes:
    """An LZO1X/1Z stream made of one literal run and the end marker (src/lzo1z.c decodes it)."""
    n = len(data)
    if n == 0:
        return b'\x11\x00\x00'
    if 4 <= n <= 238:
        return bytes([17 + n]) + data + b'\x11\x00\x00'
    if n < 4:
        raise ValueError('a literal run of fewer than 4 bytes needs a match before it')
    rest = n - 3
    if rest <= 15:
        head = bytes([rest])
    else:
        rest -= 15
        zeros, last = divmod(rest - 1, 255)
        head = b'\x00' + b'\x00' * zeros + bytes([last + 1])
    return head + data + b'\x11\x00\x00'


def lz4_compress(data: bytes) -> bytes:
    """Historical name: now an LZ40 stream (src/platform/dreamcast/dcfmv/lz40.h decodes it)."""
    return lz40_compress(data)


class Pack:
    def __init__(self) -> None:
        self.blocks: list[tuple[int, str, bytes, int]] = []
        self.ids: set[tuple[int, str]] = set()

    def add(self, rid: int, rtype: str, data: bytes, lz4: bool = False, lz40: bool = False) -> int:
        """lz40 (historical name lz4): store it LZ40-compressed if that is smaller (fewer bytes off the disc, a quick
        SH-4 decode). Returns the stored size."""
        want = lz40 or lz4
        key = (rid & 0xFFFFFFFF, rtype)
        if key in self.ids:
            raise ValueError(f'{rtype} {rid:08X} added twice')
        self.ids.add(key)
        data = pad32(data)
        packed = pad32(lz40_compress(data)) if want else data
        if len(packed) < len(data):
            self.blocks.append((rid & 0xFFFFFFFF, rtype + '+lz40', packed, len(data)))
        else:
            self.blocks.append((rid & 0xFFFFFFFF, rtype, data, len(data)))
        return len(self.blocks[-1][2])

    def __contains__(self, key: tuple[int, str]) -> bool:
        return key in self.ids

    def write(self, path) -> int:
        n = len(self.blocks)
        table_end = 0x10 + (n + 1) * 16
        text = ''.join(f'{t}={i:08X}\n' for i, t, _, _ in self.blocks).encode()
        cdir = lzo_literals(text)
        dir_off = table_end
        first = (dir_off + len(cdir) + 31) & ~31
        offs, off = [], first
        for _, _, d, _ in self.blocks:
            offs.append(off)
            off += len(d)
        out = bytearray(b'HEADLIST' + struct.pack('<II', dir_off, len(text)))
        for (rid, _, d, full), o in zip(self.blocks, offs):   # the size is the unpacked one: stored = gap to the next
            out += f'{rid:08X}'.encode() + struct.pack('<II', o, full)
        out += b'\0' * 16
        out += cdir
        out += b'\0' * (first - len(out))
        for _, _, d, _ in self.blocks:
            out += d
        with open(path, 'wb') as f:
            f.write(out)
        return len(out)
