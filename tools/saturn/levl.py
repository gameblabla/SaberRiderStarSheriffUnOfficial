"""Read a level block ("LEVL", src/level.c level_load) and the tile banks its layers use (texprep's .srgb dumps)."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import struct

import numpy as np


@dataclass
class Layer:
    name: str
    is_tilemap: bool
    parallax: float
    extra: int                      # 1: wraps horizontally (and auto-scrolls)
    w: int = 0                      # map size in tiles
    h: int = 0
    cblock: int = 0
    cells: np.ndarray | None = None  # u32 per tile: 0 empty, else cell index + 1 (bit 31: mirrored)
    used_w: int = 0


@dataclass
class Level:
    id: int
    layers: list[Layer] = field(default_factory=list)
    width: float = 0
    height: float = 0
    cols: int = 0
    rows: int = 0


def load(path: Path) -> Level:
    d = path.read_bytes()
    assert d[:4] in (b'LEVL', b'LEVh')
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    i32 = lambda o: struct.unpack_from('<i', d, o)[0]
    f32 = lambda o: struct.unpack_from('<f', d, o)[0]
    L = Level(int(path.stem, 16))
    npacks, nobjs = u32(8), u32(12)
    p = 0x10 + npacks * 12 + nobjs * 128
    nlayers = u32(p); p += 4
    flags, par, extra, names = p, p + 64, p + 128, p + 192
    for i in range(nlayers):
        name = d[names + i * 16:names + i * 16 + 15].split(b'\0')[0].decode('latin1')
        L.layers.append(Layer(name, i32(flags + i * 4) != 0, f32(par + i * 4), i32(extra + i * 4)))
    p += 192 + 15 * 16 + 16 + 4
    L.width, L.height = f32(p), f32(p + 4)
    L.cols, L.rows = u32(p + 8), u32(p + 12)
    p += 24 + L.cols * L.rows * 2
    for ly in L.layers:
        if not ly.is_tilemap or p + 12 > len(d):
            continue
        ly.w, ly.h, ly.cblock = u32(p), u32(p + 4), u32(p + 8)
        ly.cells = np.frombuffer(d, '<u4', ly.w * ly.h, p + 12).reshape(ly.h, ly.w).copy()
        p += 12 + ly.w * ly.h * 4
        ly.used_w = ly.w
        if ly.extra == 1:
            nz = np.nonzero(ly.cells.any(0))[0]
            ly.used_w = int(nz[-1]) + 1 if len(nz) else ly.w
    return L


@dataclass
class Bank:
    """a cblock as a tile bank: tile t is the tw x th rectangle at sheet position t (texprep's cblock layout)"""
    id: int
    px: np.ndarray        # RGBA sheet
    tw: int
    th: int
    ntiles: int
    sheet_cols: int
    cells: np.ndarray     # map cell index -> tile (0xFFFF: none)

    def tile(self, t: int) -> np.ndarray:
        x, y = (t % self.sheet_cols) * self.tw, (t // self.sheet_cols) * self.th
        return self.px[y:y + self.th, x:x + self.tw]


def load_bank(srgb: Path) -> Bank:
    import texbake
    kind, px, meta = texbake.load_srgb(srgb)
    assert kind == 2
    frames, cols, rows, tw, th, ntiles, sheet_cols = struct.unpack_from('<7H', meta)
    cells = np.frombuffer(meta, '<u2', frames * cols * rows, 16)
    return Bank(int(srgb.stem, 16), px, tw, th, ntiles, sheet_cols, cells)


def layer_offset(ly: Layer, cam_x: float, ticks_ms: int = 0) -> float:
    """level.c level_draw_layer: the layer's horizontal scroll for a camera position"""
    if ly.extra == 1:
        ox = (ticks_ms * 60 // 1000) * ly.parallax
        mapw = ly.used_w * 16
        return ox % mapw if mapw else 0.0
    return max(0.0, cam_x * ly.parallax)


def render_layer(ly: Layer, bank: Bank, ox: float, sw: int, sh: int, out: np.ndarray | None = None) -> np.ndarray:
    """one tile layer as level_draw_layer draws it (cam_y 0), alpha-composited over out (RGBA uint8)"""
    if out is None:
        out = np.zeros((sh, sw, 4), np.uint8)
    tw, th = bank.tw, bank.th
    wrap = ly.extra == 1
    fx = int(np.floor(-ox))
    cx0 = int(np.floor(ox / tw))
    ncells = len(bank.cells)
    for cy in range(min(ly.h, sh // th + 2)):
        for cx in range(cx0, cx0 + sw // tw + 2):
            mx = cx % ly.used_w if wrap else cx
            if mx < 0 or mx >= ly.w:
                continue
            raw = int(ly.cells[cy, mx])
            v = raw & 0x7FFFFFFF
            if not v:
                continue
            t = int(bank.cells[(v - 1) % ncells])
            if t == 0xFFFF:
                continue
            x, y = cx * tw + fx, cy * th
            tile = bank.tile(t)[:, ::-1] if raw >> 31 else bank.tile(t)
            x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + tw, sw), min(y + th, sh)
            if x1 <= x0 or y1 <= y0:
                continue
            src = tile[y0 - y:y1 - y, x0 - x:x1 - x]
            m = src[..., 3:4] >= 128
            out[y0:y1, x0:x1] = np.where(m, src, out[y0:y1, x0:x1])
    return out


def png_bank(path: Path, bank_id: int, tw: int = 16, th: int = 16) -> Bank:
    """a PNG tile sheet as gfx.c cblock_from_png makes it: tiles in reading order, map cell index = tile"""
    from PIL import Image
    px = np.asarray(Image.open(path).convert('RGBA'))
    cols, rows = px.shape[1] // tw, px.shape[0] // th
    return Bank(bank_id, px, tw, th, cols * rows, cols, np.arange(cols * rows, dtype=np.uint16))


def load_dump(path: Path) -> tuple[Level, dict[int, Path | None]]:
    """a stage's tile layers as the game built them (SABER_DUMPLAYERS, level.c level_dump_layers): the level (its id is
    the stage's plane set) and per tile bank its PNG sheet (None: a pack bank, texprep's .srgb)"""
    lines = path.read_text().splitlines()
    head = lines[0].split()
    L = Level(int(head[3], 16))
    L.width = float(head[5])
    files: dict[int, Path | None] = {}
    i = 1
    while i < len(lines):
        f = lines[i].split()
        i += 1
        assert f[0] == 'layer'
        name = ' '.join(f[2:f.index('map')])
        rest = f[f.index('map'):]
        kv = dict(zip(rest[0::2], rest[1::2]))
        ly = Layer(name, kv['map'] == '1', float(kv['parallax']), int(kv['extra']))
        L.layers.append(ly)
        if not ly.is_tilemap:
            continue
        ly.w, ly.h, ly.used_w, ly.cblock = int(kv['w']), int(kv['h']), int(kv['used_w']), int(kv['cblock'], 16)
        rows = [[int(v, 16) for v in lines[i + r].split()] for r in range(ly.h)]
        i += ly.h
        ly.cells = np.array(rows, np.uint32).reshape(ly.h, ly.w)
        files[ly.cblock] = None if kv['file'] == '-' else Path(kv['file'])
    return L, files
