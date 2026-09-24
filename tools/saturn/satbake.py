"""Bake an RGBA image into a Saturn texture block ("SAT1", read by src/platform/saturn/render_sat.c).

Block header (the first 24 bytes little-endian and laid out as the Dreamcast's "PVT1", which src/gfx.c reads for the
size and the layout data; the rest is the SH-2's big-endian):
  0 "SAT1"   4 u16 w, h   8 u16 nparts, npal   12 u16 flags (bit 0: no pixels yet), u16 0   16 u32 meta_off, meta_len
  24 u32 0, 0
  meta: the game's layout data (gfx.c: sprite frames, a cblock's cell grid), 32-byte aligned.

This is the bring-up version: the size and layout only (the null renderer draws nothing). The pixel formats
(4bpp / 8bpp colour-bank parts, palette packing, plan 4.5) come with the renderer.
"""
from __future__ import annotations

import struct

import numpy as np

import texbake   # tools/dc: load_srgb

load_srgb = texbake.load_srgb
FLAG_STUB = 1


def pad32(b: bytes) -> bytes:
    return b + b'\0' * (-len(b) % 32)


class Stats:
    def __init__(self) -> None:
        self.n = 0
        self.pixels = 0

    def report(self) -> str:
        return f'textures: {self.n}, {self.pixels * 2 // 1024} KB at 16 bpp'


def bake(px: np.ndarray, meta: bytes, stats: Stats, name: str = '') -> bytes:
    h, w = px.shape[:2]
    stats.n += 1
    stats.pixels += w * h
    head = b'SAT1' + struct.pack('<HHHHHHII8x', w, h, 0, 0, FLAG_STUB, 0, 32 if meta else 0, len(meta))
    return pad32(head + meta)
