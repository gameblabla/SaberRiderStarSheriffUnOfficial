#!/usr/bin/env python3
"""Round-trip the Saturn hero cell imports, including transparent VDP1 padding.
Run from the repository root: python3 tools/saturn/tests/hero_cells.py
"""
from pathlib import Path
import struct
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'tools/saturn'), str(ROOT / 'tools/dc')]
import build_disc
import pckwrite
import satbake
from lz40 import lz40s_decompress

pckwrite.CODEC = 'lz40s'
for name in ('saber.png', 'april.png', 'colt.png'):
    original = np.array(Image.open(ROOT / 'assets' / name).convert('RGBA'))
    source = original.copy()
    px = build_disc.hero_pixels(name, source)
    assert np.array_equal(original, source), 'import changed the source pixels'
    if name == 'saber.png':
        expected = original.copy()
        for first in (80, 104):
            for k, bob in enumerate((0, 1, 2, 0, 1, 2)):
                y, x = divmod(first + k, 8)
                expected[y * 64:y * 64 + 38 + bob, x * 64:(x + 1) * 64] = 0
        assert np.array_equal(px, expected), 'import changed pixels outside the run hip masks'
    else:
        assert np.array_equal(px, original)
    block = satbake.bake(px, b'', satbake.Stats(), name=name, rects=build_disc.atlas_rects(name))
    w, h, nu, npart, _, npal, _, _, uoff, poff = struct.unpack_from('<6H4I', block, 4)
    assert nu == w // 64 * (h // 64)
    palette = np.frombuffer(block, dtype='>u2', count=npal, offset=poff + npart * 20)
    expected = satbake.rgb555(px)
    for ui in range(nu):
        ux, uy, uw, uh, first, count = struct.unpack_from('>6H', block, uoff + ui * 12)
        assert (uw, uh) == (64, 64)
        assert (ux, uy) == (ui % (w // 64) * 64, ui // (w // 64) * 64)
        rebuilt = np.zeros((64, 72), np.uint16)
        for pi in range(first, first + count):
            x, y, pw, ph, pad, fmt, _, off, size = struct.unpack_from('>5H2B2I', block, poff + pi * 20)
            assert ux <= x and uy <= y and x + pw <= ux + 64 and y + ph <= uy + 64
            raw = block[off:off + size]
            if fmt & satbake.FMT_LZ:
                raw = lz40s_decompress(raw)
            fmt &= 0x7f
            if fmt == satbake.FMT_4BPP:
                lut = np.frombuffer(raw[:32], dtype='>u2')
                packed = np.frombuffer(raw[32:], dtype=np.uint8)
                indices = np.column_stack((packed >> 4, packed & 15)).reshape(ph, pad)
                values = lut[indices]
            elif fmt == satbake.FMT_8BPP:
                indices = np.frombuffer(raw, dtype=np.uint8).reshape(ph, pad)
                values = np.concatenate((np.array([0], dtype=np.uint16), palette))[indices]
            else:
                values = np.frombuffer(raw, dtype='>u2').reshape(ph, pad)
            assert not values[:, pw:].any(), 'nontransparent VDP1 width padding'
            rebuilt[y - uy:y - uy + ph, x - ux:x - ux + pad] |= values
        assert np.array_equal(rebuilt[:, :64], expected[uy:uy + 64, ux:ux + 64]), (name, ui)
        assert not rebuilt[:, 64:].any()
    print(f'{name}: {nu} isolated cells round-trip exactly to RGB555, with transparent padding')
