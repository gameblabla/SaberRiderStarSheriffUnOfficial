"""Extract the bundled HuC boot header. No BIOS or commercial disc is copied.

The header's compressed form and decompressor ship in HuC's isolink/main.c.
Keep that source as the dependency/provenance rather than a binary in git.
"""
import re
import sys
from pathlib import Path


def header(source: Path) -> bytes:
    text = source.read_text()
    block = re.search(r'disc_header_pce\s*\[\d+\]\s*=\s*\{(.*?)\};', text, re.S)
    if not block:
        raise ValueError('HuC IPL header not found')
    packed = bytes(int(n, 16) for n in re.findall(r'0x([0-9A-Fa-f]{2})', block[1]))
    p, out = 0, bytearray()
    def byte():
        nonlocal p
        b = packed[p]; p += 1
        return b
    def word():
        return byte() | byte() << 8
    while True:
        cmd = byte()
        length = (cmd & 112) >> 4
        if length == 7:
            length = byte() + 7
            if length == 256: length = word()
            elif length == 257: length = byte() + 256
        for _ in range(length): out.append(byte() ^ 0xaa)
        offset = byte() - 256
        if cmd & 128: offset = (offset & 255) | byte() << 8; offset -= 65536
        length = (cmd & 15) + 3
        if length == 18:
            length = byte() + 18
            if length == 256:
                length = word()
                if not length: break
            elif length == 257: length = byte() + 256
        for _ in range(length): out.append(out[len(out) + offset])
    if len(out) < 2048: raise ValueError('Truncated IPL')
    return bytes(out[:2048])


if __name__ == '__main__':
    Path(sys.argv[2]).write_bytes(header(Path(sys.argv[1])))
