"""Write E2DM "HEADLIST" packs (the format src/pack.c reads) for the Dreamcast disc.

Every block is stored raw (no LZO), starts on a 32-byte boundary and is a multiple of 32 bytes long, so the game reads
one straight into a 32-byte aligned buffer in a single read and can DMA it on to video or sound memory. The packer
lays blocks out in the order they are added: put the ones a stage loads together next to each other.

Layout: "HEADLIST", directory offset, directory length (unpacked); 16-byte entries from 0x10 (8 hex digits of id,
offset, size) ended by a zero entry; the LZO1Z-coded "type=ID\\n" directory; the blocks.
"""
from __future__ import annotations

import struct


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


class Pack:
    def __init__(self) -> None:
        self.blocks: list[tuple[int, str, bytes]] = []
        self.ids: set[tuple[int, str]] = set()

    def add(self, rid: int, rtype: str, data: bytes) -> None:
        key = (rid & 0xFFFFFFFF, rtype)
        if key in self.ids:
            raise ValueError(f'{rtype} {rid:08X} added twice')
        self.ids.add(key)
        self.blocks.append((rid & 0xFFFFFFFF, rtype, pad32(data)))

    def __contains__(self, key: tuple[int, str]) -> bool:
        return key in self.ids

    def write(self, path) -> int:
        n = len(self.blocks)
        table_end = 0x10 + (n + 1) * 16
        text = ''.join(f'{t}={i:08X}\n' for i, t, _ in self.blocks).encode()
        cdir = lzo_literals(text)
        dir_off = table_end
        first = (dir_off + len(cdir) + 31) & ~31
        offs, off = [], first
        for _, _, d in self.blocks:
            offs.append(off)
            off += len(d)
        out = bytearray(b'HEADLIST' + struct.pack('<II', dir_off, len(text)))
        for (rid, _, d), o in zip(self.blocks, offs):
            out += f'{rid:08X}'.encode() + struct.pack('<II', o, len(d))
        out += b'\0' * 16
        out += cdir
        out += b'\0' * (first - len(out))
        for _, _, d in self.blocks:
            out += d
        with open(path, 'wb') as f:
            f.write(out)
        return len(out)
