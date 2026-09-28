#!/usr/bin/env python3
"""Test the C tile partitioner and audit platformer fill using actual baked alpha.

python3 tools/dc/tile_regions_test.py
python3 tools/dc/tile_regions_test.py --tex-pack build/dc/stage/data/tex.pck

No emulator or GPU timing model: the optional audit counts submitted rectangles,
checks nearest-sampled alpha against unsplit tiles, and bounds RAM stream usage.
Requires a host C compiler; the asset audit also uses the baker's numpy dependency.
"""
import argparse
import ctypes as ct
import itertools
import math
from pathlib import Path
import shlex
import os
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OP, PT, TR, EMPTY = range(4)


class Region(ct.Structure):
    _fields_ = [(k, ct.c_uint8) for k in ('x', 'y', 'w', 'h', 'cls')]


def partitions(work):
    wrapper = work / 'regions.c'
    wrapper.write_text('''#include "pvr_tile_regions.h"
int partition(const uint8_t *cls, PvrTileRegion *out) { return pvr_tile_regions(cls, out); }
int classify(unsigned bits) { return am_class(bits); }
''')
    libpath = work / 'regions.so'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
        '-I', str(ROOT / 'src/platform/dreamcast'), str(wrapper), '-o', str(libpath)], check=True)
    lib = ct.CDLL(str(libpath))
    lib.partition.argtypes = [ct.POINTER(ct.c_uint8), ct.POINTER(Region)]
    lib.classify.argtypes = [ct.c_uint]
    for bits, expected in [(0, OP), (2, OP), (1, EMPTY), (3, PT), (7, TR)]:
        assert lib.classify(bits) == expected
    table = {}
    for classes in itertools.product(range(4), repeat=4):
        out = (Region * 4)()
        n = lib.partition((ct.c_uint8 * 4)(*classes), out)
        assert 1 <= n <= 4
        seen = [None] * 4
        parts = []
        for q in out[:n]:
            assert 0 <= q.x < 2 and 0 <= q.y < 2 and 1 <= q.w <= 2 and 1 <= q.h <= 2
            assert q.x + q.w <= 2 and q.y + q.h <= 2
            for y in range(q.y, q.y + q.h):
                for x in range(q.x, q.x + q.w):
                    i = y * 2 + x
                    assert seen[i] is None, (classes, 'overlap')
                    seen[i] = q.cls
            parts.append((q.x * 8, q.y * 8, q.w * 8, q.h * 8, q.cls))
        assert tuple(seen) == classes, (classes, 'hole or wrong class')
        if len(set(classes)) == 1:
            assert n == 1
        table[classes] = parts
    print('C partitioner: all 256 class combinations cover each texel exactly once; uniform tiles stay one quad')
    return table


def alpha_class(a):
    if (a == 255).all():
        return OP
    if (a == 0).all():
        return EMPTY
    if ((a == 0) | (a == 255)).all():
        return PT
    return TR


def read_pack(path):
    data = path.read_bytes()
    assert data[:8] == b'HEADLIST'
    blocks = {}
    pos = 16
    while data[pos:pos + 8] != bytes(8):
        rid = int(data[pos:pos + 8], 16)
        off, size = struct.unpack_from('<II', data, pos + 8)
        blocks[rid] = data[off:off + size]
        pos += 16
    return blocks


def baked_alpha(block):
    import numpy as np
    import texbake
    assert block[:4] == b'PVT1', 'audit needs uncompressed PVT1 stage tile sheets'
    w, h, pages, ncol = struct.unpack_from('<4H', block, 4)
    paloff = struct.unpack_from('<I', block, 24)[0]
    palette = np.frombuffer(block, '<u4', ncol, paloff) >> 24
    alpha = np.zeros((h, w), np.uint8)
    for i in range(pages):
        x, y, pw, ph, tw, th, fmt, off, length = struct.unpack_from('<6H3I', block, 32 + i * 32)
        assert not fmt & 0x100, 'VQ has no runtime alpha map; cannot audit it as a split tile'
        raw = np.frombuffer(block, np.uint8, length, off)
        if fmt in (5, 6):
            indices = raw if fmt == 6 else np.stack((raw & 15, raw >> 4), axis=1).ravel()
            flat = palette[indices[:tw * th]]
        else:
            pixels = np.frombuffer(block, '<u2', tw * th, off)
            flat = np.full(tw * th, 255) if fmt == 1 else (pixels >> 15) * 255 if fmt == 0 else (pixels >> 12) * 17
        page = flat[texbake.twiddle_index(tw, th)]
        alpha[y:y + ph, x:x + pw] = page[:ph, :pw]
    return alpha


def check_samples(alpha, parts):
    """Independent nearest-sample oracle for cut UVs, both flips and clipped edges.
    Compare coverage and sampled alpha; partitioning must not alter texture colour
    coordinates, and only zero-alpha regions may disappear with blending enabled.
    """
    import numpy as np
    for scale, flip in itertools.product((1.0, 1.5, 2.0), (False, True)):
        edge = int(16 * scale)
        y, x = np.mgrid[:edge, :edge].astype(float) + 0.5
        # Offset clipping at the display border, with the last row/column clipped too.
        for left, top in ((0, 0), (3, 5)):
            clip = (x >= left) & (y >= top) & (x < edge - left) & (y < edge - top)
            u = np.floor(16 - x / scale if flip else x / scale).astype(int)
            v = np.floor(y / scale).astype(int)
            expected = np.where(clip, alpha[v, u], 0)
            result = np.zeros_like(expected)
            hits = np.zeros_like(expected)
            for sx, sy, w, h, cls in parts:
                if cls == EMPTY:
                    continue
                dx = 16 - sx - w if flip else sx
                inside = clip & (x >= dx * scale) & (x < (dx + w) * scale) & (y >= sy * scale) & (y < (sy + h) * scale)
                # Interpolate the region's own endpoints, independently of the full tile's UVs.
                ru = sx + w - (x - dx * scale) / scale if flip else sx + (x - dx * scale) / scale
                rv = sy + (y - sy * scale) / scale
                ix, iy = np.floor(ru[inside]).astype(int), np.floor(rv[inside]).astype(int)
                assert np.array_equal(ix, u[inside]) and np.array_equal(iy, v[inside])
                sampled = alpha[iy, ix]
                if cls == OP:
                    assert (sampled == 255).all()
                if cls == PT:
                    assert ((sampled == 0) | (sampled == 255)).all()
                result[inside] = sampled
                hits[inside] += 1
            assert (hits <= 1).all() and np.array_equal(result, expected)


def audit_stage(folder, blocks, table):
    import numpy as np
    from build_disc import namehash
    data = (ROOT / 'assets' / folder / f'{folder}.lvl').read_bytes()
    assert data[:4] == b'FRST'
    level_width = struct.unpack_from('<I', data, 8)[0]
    nlayers = struct.unpack_from('<I', data, 24)[0]
    layers, off, verified = [], 28, 0
    for _ in range(nlayers):
        name, png, rate, w, h, tile = struct.unpack_from('<16s16sfIII', data, off)
        off += 48
        assert tile == 16
        name = name.rstrip(b'\0').decode()
        png = png.rstrip(b'\0').decode()
        cells = np.frombuffer(data, '<u4', w * h, off).reshape(h, w)
        off += w * h * 4
        alpha = baked_alpha(blocks[namehash(f'{folder}/{png}')])
        tiles, cols = {}, alpha.shape[1] // 16
        for cell in np.unique(cells):
            if cell == 0:
                continue
            y, x = divmod(int(cell) - 1, cols)
            a = alpha[y * 16:y * 16 + 16, x * 16:x * 16 + 16]
            assert a.shape == (16, 16)
            cls = alpha_class(a)
            classes = tuple(alpha_class(a[dy:dy + 8, dx:dx + 8]) for dy in (0, 8) for dx in (0, 8))
            parts = table[classes] if cls in (PT, TR) else [(0, 0, 16, 16, cls)]
            check_samples(a, parts)
            tiles[int(cell)] = (cls, parts)
            verified += 1
        layers.append((name, rate, cells, tiles))
    print(f'{folder}: verified alpha/UV/coverage for {verified} used baked tiles at 1x, 1.5x, 2x, mirrored and clipped')
    # Sweep the full route in 8-pixel camera steps, including fractional parallax offsets.
    for width in (320, 416, 426):
        peak_bytes = np.zeros(3, int)
        totals = np.zeros((2, 4), float)
        count = 0
        for cam in range(0, max(1, level_width - width + 1), 8):
            area = np.zeros((2, 4), float)
            quads = np.zeros(3, int)
            for name, rate, cells, tiles in layers:
                scroll = math.ceil(cam * rate)
                first = scroll // 16
                for cy in range(min(15, cells.shape[0])):
                    for cx in range(first, min(cells.shape[1], first + width // 16 + 2)):
                        cell = int(cells[cy, cx])
                        if cell == 0:
                            continue
                        cls, parts = tiles[cell]
                        dx, dy = cx * 16 - scroll, cy * 16
                        visible = max(0, min(width, dx + 16) - max(0, dx)) * 16
                        area[0, cls] += visible
                        for x, y, w, h, c in parts:
                            visible = max(0, min(width, dx + x + w) - max(0, dx + x)) * h
                            area[1, c] += visible
                            if c != EMPTY and visible:
                                quads[c] += 1
            if cam == 0:
                print(f'  {width} opening OP/PT/TR screens: {area[0, :3] / (width * 240)} -> {area[1, :3] / (width * 240)}')
            # Conservative upper bound: a fresh 32-byte header for every 128-byte quad.
            peak_bytes = np.maximum(peak_bytes, quads * 160)
            totals += area / (width * 240)
            count += 1
        assert peak_bytes[OP] < 256 * 1024 and peak_bytes[PT] < 160 * 1024, peak_bytes
        print(f'  {width} route mean PT+TR screens: {totals[0, PT:TR + 1].sum() / count:.3f} -> '
              f'{totals[1, PT:TR + 1].sum() / count:.3f}; max OP/PT streams <= {peak_bytes[:2]} bytes (tiles only)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tex-pack', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='saber-pvr-') as tmp:
        table = partitions(Path(tmp))
    if args.tex_pack:
        blocks = read_pack(args.tex_pack)
        for folder in ('forest', 'lab'):
            audit_stage(folder, blocks, table)


if __name__ == '__main__':
    main()
