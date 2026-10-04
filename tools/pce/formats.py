"""PC Engine native formats. Byte addresses are used in files; VDC addresses
are words. All integers are little endian, all asset offsets explicitly u32.
"""
from __future__ import annotations
import struct
import numpy as np
from PIL import Image

ARCADE_BYTES = 0x1e0000 # final 128 KiB is the renderer's tile directory
BG_TILES = 896 # reserve $4000-$41ff for dialogue BG characters
BG_WORD = 0x0800
SPR_WORD = 0x4800
SAT_WORD = 0x7f00

def vce_colors(rgb):
    v = (np.asarray(rgb, dtype=np.uint16) * 7 + 127) // 255
    return (v[..., 1] << 6 | v[..., 0] << 3 | v[..., 2]).astype('<u2')

def vce_rgb(words):
    w = np.asarray(words, dtype=np.uint16)
    return np.stack(((w >> 3) & 7, (w >> 6) & 7, w & 7), -1).astype(np.uint16) * 255 // 7

def planar_tile(index):
    """8x8 character: low planes interleaved, then high planes interleaved."""
    a = np.asarray(index, dtype=np.uint8)
    if a.shape != (8, 8) or a.max() > 15: raise ValueError('Invalid BG character')
    planes = [np.packbits((a >> b) & 1, axis=1).ravel() for b in range(4)]
    return np.stack((planes[0], planes[1]), -1).tobytes() + np.stack((planes[2], planes[3]), -1).tobytes()

def planar_sprite(index):
    """16x16 sprite: four 32-byte planes, right byte first in each word."""
    a = np.asarray(index, dtype=np.uint8)
    if a.shape != (16, 16) or a.max() > 15: raise ValueError('Invalid sprite pattern')
    return b''.join(np.packbits((a >> b) & 1, axis=1)[:, ::-1].tobytes() for b in range(4))

def palette_for(images, colors=15, unique=False):
    """`unique`: every distinct colour counts once, so a few pixels of a rare colour keep their own entry."""
    px = np.concatenate([np.asarray(im.convert('RGBA')).reshape(-1, 4) for im in images])
    px = px[px[:, 3] >= 128, :3]
    if not len(px): return np.zeros(16, '<u2')
    # Quantization occurs on the final 9-bit color lattice.
    px = vce_rgb(vce_colors(px)).astype(np.uint8)
    if unique: px = np.unique(px, axis=0)
    q = Image.fromarray(px.reshape(1, -1, 3)).quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    pal = np.zeros(16, '<u2')
    rgb = np.asarray(q.getpalette(), np.uint8).reshape(-1, 3)[:colors]
    pal[1:len(rgb) + 1] = vce_colors(rgb)
    return pal

_index_luts = {}
def indexed(image, palette):
    a = np.asarray(image.convert('RGBA'))
    key = palette.tobytes()
    if key not in _index_luts:
        rgb = vce_rgb(palette[1:]).astype(np.int16)
        # LUT on the 512 hardware colors avoids a large per-pixel temporary.
        lattice = vce_rgb(np.arange(512)).astype(np.int16)
        delta = lattice[:, None, :].astype(np.int32) - rgb[None, :, :]
        _index_luts[key] = np.argmin((delta * delta).sum(-1), axis=1).astype(np.uint8) + 1
    lut = _index_luts[key]
    return np.where(a[..., 3] >= 128, lut[vce_colors(a[..., :3])], 0).astype(np.uint8)

def pack_sprite(image, anchor=(0, 0), palette=None):
    if palette is None: palette = palette_for([image])
    idx = indexed(image, palette)
    h, w = idx.shape
    # Start each horizontal strip at its first opaque pixel. A padded source
    # cell can otherwise straddle three hardware columns despite fitting two.
    # Keep strips on their original Y grid: this preserves foreground bands,
    # source placement and hardware mirroring exactly.
    patterns, pieces = [], []
    for y in range(0, h, 16):
        strip = idx[y:y + 16]
        occupied = np.flatnonzero(strip.any(axis=0))
        if not len(occupied): continue
        for x in range(int(occupied[0]), int(occupied[-1]) + 1, 16):
            cell = strip[:, x:x + 16]
            cell = np.pad(cell, ((0, 16-cell.shape[0]), (0, 16-cell.shape[1])))
            if not cell.any(): continue
            pieces.append((x - anchor[0], y - anchor[1], len(patterns)))
            patterns.append(planar_sprite(cell))
    if len(pieces) > 64: raise ValueError(f'Sprite exceeds SAT: {len(pieces)}')
    occupancy = np.zeros(h + 32, dtype=np.uint8)
    for _, y, _ in pieces:
        sy = y + anchor[1]
        occupancy[sy:sy + 16] += 1
    return b''.join(patterns), pieces, palette, int(occupancy.max(initial=0))

def pair_characters():
    return b''.join(planar_tile(np.tile([v >> 4] * 4 + [v & 15] * 4, (8, 1))) for v in range(256))

def align(data, size=2048):
    return data + bytes(-len(data) % size)

class Archive:
    def __init__(self): self.data = bytearray(); self.records = {}
    def add(self, name, data, alignment=2):
        self.data.extend(bytes(-len(self.data) % alignment))
        offset = len(self.data)
        self.data.extend(data)
        self.records[name] = dict(offset=offset, bytes=len(data))
        if len(self.data) > ARCADE_BYTES: raise ValueError(f'{name}: Arcade working set exceeds 2 MiB')
        return offset
    def finish(self): return align(bytes(self.data))
