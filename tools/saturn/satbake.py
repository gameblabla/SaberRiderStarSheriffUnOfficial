"""Bake an RGBA image into a Saturn texture block ("SAT1", read by src/platform/saturn/render_sat.c).

A texture is cut into the rectangles the game draws out of it ("units": a sprite's frames, a cblock's tiles, a font's
glyphs, an atlas' entries, else the whole image). VDP1 can only draw a whole stored sprite, so each unit is stored as
its own "parts": the unit's opaque bounding box, cut into horizontal bands where needed, each band either
  * 4bpp with a 16-entry colour lookup table (VDP1 "LUT" mode: entry 0 transparent, 1..15 RGB555) when it has at
    most 15 colours (exact: the demo's art is 15-bit), or
  * 16bpp RGB555 (bit 15 set on opaque pixels, 0 transparent) when it has more,
stored LZ4-compressed where that is smaller. Widths are padded to multiples of 8 (VDP1's unit) with transparent
pixels. A band holds at most MAX_RAW bytes (the renderer decodes a part into a staging buffer of that size).

Block layout (the 32-byte header little-endian, its first 24 bytes laid out as the Dreamcast's "PVT1": src/gfx.c
reads the size and the layout data from them; everything after the header is big-endian, the SH-2's order):
  0 "SAT1"   4 u16 w, h   8 u16 nunits, nparts   12 u16 flags, 0   16 u32 meta_off, meta_len   24 u32 units_off, parts_off
  units (12 bytes each, sorted by y then x): u16 x, y, w, h, first_part, nparts
  parts (20 bytes each): u16 x, y (in the texture), w (true), h, wpad; u8 fmt (0 4bpp+LUT, 1 16bpp; bit 7: LZ4),
         u8 0; u32 data_off (from the block's start), data_len (stored bytes); raw size = 32 + wpad*h/2 (4bpp: the
         LUT first) or wpad*h*2
  meta: the game's layout data (gfx.c), then the part data; every section 4-byte aligned.
"""
from __future__ import annotations

import struct

import numpy as np

import texbake   # tools/dc: load_srgb, lz4 via pckwrite
import pckwrite

load_srgb = texbake.load_srgb
MAX_RAW = 32 * 1024
FMT_4BPP, FMT_16BPP, FMT_LZ4 = 0, 1, 0x80


def pad4(b: bytes) -> bytes:
    return b + b'\0' * (-len(b) % 4)


def rgb555(px: np.ndarray) -> np.ndarray:
    """RGBA8 -> RGB555 with bit 15 set on opaque pixels (alpha >= 128), 0 on transparent ones"""
    r = (px[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (px[..., 1].astype(np.uint32) * 31 + 127) // 255
    b = (px[..., 2].astype(np.uint32) * 31 + 127) // 255
    v = 0x8000 | (b << 10) | (g << 5) | r
    return np.where(px[..., 3] >= 128, v, 0).astype(np.uint16)


class Stats:
    def __init__(self) -> None:
        self.n = 0
        self.units = 0
        self.parts = {FMT_4BPP: 0, FMT_16BPP: 0}
        self.raw = {FMT_4BPP: 0, FMT_16BPP: 0}
        self.stored = 0
        self.src_pixels = 0

    def report(self) -> str:
        return (f'textures: {self.n}, units {self.units}, parts 4bpp {self.parts[FMT_4BPP]} ({self.raw[FMT_4BPP] // 1024} KB) '
                f'16bpp {self.parts[FMT_16BPP]} ({self.raw[FMT_16BPP] // 1024} KB), stored {self.stored // 1024} KB '
                f'(source {self.src_pixels * 2 // 1024} KB at 16 bpp)')


def cut_bands(v: np.ndarray) -> list[tuple[int, int, int]]:
    """rows of a unit's RGB555 pixels -> (y0, y1, fmt) bands: 4bpp runs where a band of rows has <= 15 colours,
    16bpp elsewhere; every band within MAX_RAW"""
    h, w = v.shape
    wpad = (w + 7) & ~7
    bands: list[tuple[int, int, int]] = []
    y = 0
    while y < h:
        colours: set[int] = set()
        y1 = y
        while y1 < h:
            row = set(np.unique(v[y1]).tolist()) - {0}
            if len(colours | row) > 15 or (y1 - y + 1) * wpad // 2 + 32 > MAX_RAW:
                break
            colours |= row
            y1 += 1
        if y1 - y >= 4 or (y1 == h and y1 > y):
            bands.append((y, y1, FMT_4BPP))
            y = y1
            continue
        # too many colours for a useful 4bpp band here: 16bpp rows until a 4bpp band of 8+ rows could start again
        y1 = y + 1
        while y1 < h and (y1 - y + 1) * wpad * 2 <= MAX_RAW:
            probe: set[int] = set()
            ok = True
            for k in range(y1, min(h, y1 + 8)):
                probe |= set(np.unique(v[k]).tolist()) - {0}
                if len(probe) > 15:
                    ok = False
                    break
            if ok:
                break
            y1 += 1
        bands.append((y, y1, FMT_16BPP))
        y = y1
    # merge neighbouring bands of one format when the result still fits (fewer VDP1 commands)
    merged: list[tuple[int, int, int]] = []
    for b in bands:
        if merged and merged[-1][2] == b[2] == FMT_16BPP and (b[1] - merged[-1][0]) * wpad * 2 <= MAX_RAW:
            merged[-1] = (merged[-1][0], b[1], FMT_16BPP)
        else:
            merged.append(b)
    return merged


def part_data(v: np.ndarray, fmt: int) -> bytes:
    h, w = v.shape
    wpad = (w + 7) & ~7
    pad = np.zeros((h, wpad), np.uint16)
    pad[:, :w] = v
    if fmt == FMT_16BPP:
        return pad.astype('>u2').tobytes()
    colours = np.array(sorted(set(np.unique(pad).tolist()) - {0}), np.uint16)
    lut = np.zeros(16, np.uint16)
    lut[1:1 + len(colours)] = colours
    idx = np.where(pad != 0, np.searchsorted(colours, pad) + 1, 0).astype(np.uint8) if len(colours) else np.zeros_like(pad, np.uint8)
    packed = (idx[:, 0::2] << 4) | idx[:, 1::2]
    return lut.astype('>u2').tobytes() + packed.astype(np.uint8).tobytes()


def units_for(w: int, h: int, meta: bytes, kind: int, rects: list[tuple[int, int, int, int]] | None) -> list[tuple[int, int, int, int]]:
    if rects:
        return rects
    if kind == 1 and len(meta) >= 6:   # sprite: frames side by side
        fw, fh, frames = struct.unpack_from('<HHH', meta)
        return [(f * fw, 0, fw, fh) for f in range(frames) if (f + 1) * fw <= w]
    if kind == 2 and len(meta) >= 16:  # cblock: tiles, sheet_cols a row
        frames, cols, rows, tw, th, ntiles, sheet_cols = struct.unpack_from('<7H', meta)
        return [((t % sheet_cols) * tw, (t // sheet_cols) * th, tw, th) for t in range(ntiles)]
    return [(0, 0, w, h)]


def bake(px: np.ndarray, meta: bytes, stats: Stats, name: str = '', kind: int = 0,
         rects: list[tuple[int, int, int, int]] | None = None) -> bytes:
    h, w = px.shape[:2]
    stats.n += 1
    stats.src_pixels += w * h
    v = rgb555(px)
    units = units_for(w, h, meta, kind, rects)
    units = sorted(set(units), key=lambda r: (r[1], r[0], r[2], r[3]))
    stats.units += len(units)
    parts: list[tuple[int, int, int, int, int, int, bytes]] = []   # x, y, w, h, wpad, fmt, data
    unit_rows = []
    cache: dict[bytes, int] = {}   # identical parts (e.g. repeated tiles) are stored once
    for ux, uy, uw, uh in units:
        sub = v[uy:uy + uh, ux:ux + uw]
        first = len(parts)
        ys, xs = np.nonzero(sub)
        if len(xs):
            x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
            box = sub[y0:y1, x0:x1]
            for b0, b1, fmt in cut_bands(box):
                data = part_data(box[b0:b1], fmt)
                parts.append((ux + x0, uy + y0 + b0, x1 - x0, b1 - b0, ((x1 - x0) + 7) & ~7, fmt, data))
                stats.parts[fmt] += 1
                stats.raw[fmt] += len(data)
        unit_rows.append((ux, uy, uw, uh, first, len(parts) - first))
    head_len = 32
    units_off = head_len
    parts_off = units_off + 12 * len(unit_rows)
    meta_off = parts_off + 20 * len(parts)
    data_off = meta_off + len(pad4(meta))
    blob = bytearray()
    prows = []
    for x, y, pw, ph, wpad, fmt, data in parts:
        key = bytes([fmt]) + data
        if key in cache:
            off, n, f = cache[key]
        else:
            packed = pckwrite.lz4_compress(data) if len(data) >= 64 else data
            f = fmt
            if len(packed) < len(data):
                f |= FMT_LZ4
            else:
                packed = data
            off, n = data_off + len(blob), len(packed)
            blob += pad4(packed)
            cache[key] = (off, n, f)
        prows.append(struct.pack('>5HBBII', x, y, pw, ph, wpad, f, 0, off, n))
    stats.stored += len(blob)
    head = b'SAT1' + struct.pack('<HHHHHHIIII', w, h, len(unit_rows), len(parts), 0, 0, meta_off if meta else 0, len(meta),
                                 units_off, parts_off)
    out = head + b''.join(struct.pack('>6H', *u) for u in unit_rows) + b''.join(prows) + pad4(meta) + bytes(blob)
    return out
