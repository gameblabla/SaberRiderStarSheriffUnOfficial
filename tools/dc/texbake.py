"""Bake an RGBA image into a Dreamcast texture block ("PVT1", read by src/platform/dreamcast/render_pvr.c).

The format is chosen per image, smallest first, and never at a visible cost:
  * <= 16 colours: 4-bit palette, <= 256 colours: 8-bit palette. Exact (the palette is ARGB8888).
  * more colours: the 16-bit format the image needs (RGB565 opaque, ARGB1555 cut-out, ARGB4444 translucent) is the
    reference. pvrtex's VQ (2 bits a texel) or its 256-colour quantisation replaces it only when its error is no
    larger than the 16-bit one's (PSNR against the source, within TOLERANCE_DB), i.e. no loss is shown.
Everything is twiddled, in power-of-two pages of at most 1024x1024 (an image is cut into a few pages instead of being
padded to the next power of two). Palette indices are local (0..n-1): the game places the colours in palette RAM
when it loads the texture and remaps the indices.

Block layout (little endian; every section 32-byte aligned):
  0  "PVT1"   4 u16 w, h   8 u16 npages, ncolors   12 u16 fmt16 (0 1555, 1 565, 2 4444: the fallback when palette RAM
     is full), u16 0   16 u32 meta_off, meta_len   24 u32 pal_off, 0
  32 npages x 32: u16 x0, y0, w, h, tw, th; u32 fmt (bits 0-2 PVR pixel format: 0 1555, 1 565, 2 4444, 5 pal4,
     6 pal8; bit 8 VQ); u32 data_off, data_len; 8 bytes 0
  palette: ncolors x u32 ARGB8888; meta: the game's layout data (gfx.c); page data (VQ: 2 KB codebook + indices).
"""
from __future__ import annotations

import os
import struct
import subprocess
import tempfile

import numpy as np
from PIL import Image

PVRTEX = '/opt/toolchains/dc/kos/utils/pvrtex/pvrtex'
TOLERANCE_DB = 0.25
FMT_1555, FMT_565, FMT_4444, FMT_PAL4, FMT_PAL8 = 0, 1, 2, 5, 6
VQ = 0x100
BPP = {FMT_1555: 16, FMT_565: 16, FMT_4444: 16, FMT_PAL4: 4, FMT_PAL8: 8}


def pad32(b: bytes) -> bytes:
    return b + b'\0' * (-len(b) % 32)


def pot(n: int) -> int:
    p = 8
    while p < n:
        p <<= 1
    return p


PAGE_COST = 12   # texels of padding worth saving a page (a page is one more quad per draw)


def segments(length: int) -> list[int]:
    """Power-of-two page sizes (8..1024) covering length with the least padding + PAGE_COST per page"""
    sizes = [8 << i for i in range(8)]
    cost, first = [0] * (length + 1), [0] * (length + 1)
    for n in range(1, length + 1):
        best = None
        for p in sizes:
            c = p + PAGE_COST + (cost[n - p] if p < n else 0)
            if best is None or c < best:
                best, first[n] = c, p
            if p >= n:
                break
        cost[n] = best
    out, n = [], length
    while n > 0:
        out.append(first[n])
        n -= first[n]
    return sorted(out, reverse=True)   # big pages first: tile and frame edges stay on page edges


def pages_for(w: int, h: int) -> list[tuple[int, int, int, int, int, int]]:
    """(x0, y0, w, h, tw, th) of each page"""
    pages = []
    y0 = 0
    for th in segments(h):
        x0 = 0
        for tw in segments(w):
            pages.append((x0, y0, min(tw, w - x0), min(th, h - y0), tw, th))
            x0 += tw
        y0 += th
    return pages


def vram_bytes(w: int, h: int, bpp: int) -> int:
    return sum(tw * th * bpp // 8 for *_, tw, th in pages_for(w, h))


_twid_cache: dict[tuple[int, int], np.ndarray] = {}


def twiddle_index(tw: int, th: int) -> np.ndarray:
    """texel (y, x) -> its index in the PVR's twiddled order (KOS pvr_txr_load_ex): the square blocks of
    min(tw, th) follow each other along the long side, inside one y takes the even bits, x the odd ones"""
    key = (tw, th)
    if key not in _twid_cache:
        mn = min(tw, th)

        def spread(v):
            r = np.zeros_like(v)
            for b in range(10):
                r |= ((v >> b) & 1) << (2 * b)
            return r
        y, x = np.mgrid[0:th, 0:tw]
        idx = spread(y & (mn - 1)) | (spread(x & (mn - 1)) << 1)
        idx = idx + (x // mn + y // mn) * mn * mn
        _twid_cache[key] = idx
    return _twid_cache[key]


def twiddle(page: np.ndarray) -> np.ndarray:
    """page (th, tw[, c]) -> flat array in twiddled order"""
    th, tw = page.shape[:2]
    out = np.empty((th * tw,) + page.shape[2:], page.dtype)
    out[twiddle_index(tw, th).ravel()] = page.reshape((th * tw,) + page.shape[2:])
    return out


def crop_page(img: np.ndarray, x0, y0, w, h, tw, th) -> np.ndarray:
    """the page's part of the image, the spare texels repeating its last row / column (bilinear filtering at the edge)"""
    part = img[y0:y0 + h, x0:x0 + w]
    return np.pad(part, ((0, th - h), (0, tw - w)) + ((0, 0),) * (img.ndim - 2), mode='edge')


def canonical(rgba: np.ndarray) -> np.ndarray:
    out = rgba.copy()
    out[out[..., 3] == 0] = 0
    return out


def alpha_kind(rgba: np.ndarray) -> int:
    a = rgba[..., 3]
    if (a == 255).all():
        return FMT_565
    if np.isin(a, (0, 255)).all():
        return FMT_1555
    return FMT_4444


def to16(rgba: np.ndarray, fmt: int) -> np.ndarray:
    r, g, b, a = (rgba[..., i].astype(np.uint32) for i in range(4))
    q = lambda v, bits: (v * ((1 << bits) - 1) + 127) // 255
    if fmt == FMT_565:
        v = q(r, 5) << 11 | q(g, 6) << 5 | q(b, 5)
    elif fmt == FMT_1555:
        v = (a >= 128).astype(np.uint32) << 15 | q(r, 5) << 10 | q(g, 5) << 5 | q(b, 5)
    else:
        v = q(a, 4) << 12 | q(r, 4) << 8 | q(g, 4) << 4 | q(b, 4)
    return v.astype(np.uint16)


def from16(v: np.ndarray, fmt: int) -> np.ndarray:
    v = v.astype(np.uint32)
    e = lambda x, bits: (x * 255 + ((1 << bits) - 1) // 2) // ((1 << bits) - 1)
    if fmt == FMT_565:
        ch = [e(v >> 11 & 31, 5), e(v >> 5 & 63, 6), e(v & 31, 5), np.full_like(v, 255)]
    elif fmt == FMT_1555:
        ch = [e(v >> 10 & 31, 5), e(v >> 5 & 31, 5), e(v & 31, 5), (v >> 15) * 255]
    else:
        ch = [e(v >> 8 & 15, 4), e(v >> 4 & 15, 4), e(v & 15, 4), e(v >> 12 & 15, 4)]
    return np.stack(ch, -1).astype(np.uint8)


def psnr(src: np.ndarray, got: np.ndarray) -> float:
    """colour error where the source is visible, plus the alpha error everywhere"""
    s, g = src.astype(np.float64), got.astype(np.float64)
    vis = src[..., 3] > 0
    err = ((s[..., :3] - g[..., :3]) ** 2).sum(-1) * vis + (s[..., 3] - g[..., 3]) ** 2
    m = err.sum() / (src.shape[0] * src.shape[1] * 4)
    return 99.0 if m == 0 else 10 * np.log10(255 * 255 / m)


def palettize(rgba: np.ndarray):
    """exact palette: (colours as uint32 RGBA, index image) or None when there are more than 256"""
    flat = rgba.reshape(-1, 4)
    packed = flat.view(np.uint32).ravel()
    colors, idx = np.unique(packed, return_inverse=True)
    if len(colors) > 256:
        return None
    return colors, idx.reshape(rgba.shape[:2]).astype(np.uint8)


def argb8888(colors: np.ndarray) -> np.ndarray:
    """RGBA-in-memory uint32s -> ARGB8888 palette entries"""
    c = colors.view(np.uint8).reshape(-1, 4).astype(np.uint32)
    return (c[:, 3] << 24 | c[:, 0] << 16 | c[:, 1] << 8 | c[:, 2]).astype('<u4')


def run_pvrtex(page: np.ndarray, args: list[str]):
    """pvrtex on one page: (preview RGBA, .dt texture data or None)"""
    with tempfile.TemporaryDirectory() as d:
        src, dt, prev = os.path.join(d, 'in.png'), os.path.join(d, 'out.dt'), os.path.join(d, 'prev.png')
        Image.fromarray(page, 'RGBA').save(src)
        subprocess.run([PVRTEX, '-i', src, '-o', dt, '-p', prev] + args, check=True, capture_output=True)
        preview = np.array(Image.open(prev).convert('RGBA'))
        with open(dt, 'rb') as f:
            blob = f.read()
    assert blob[:4] == b'DcTx', 'pvrtex .dt'
    hsize = (blob[9] + 1) * 32
    size = struct.unpack_from('<I', blob, 4)[0]
    data = blob[hsize:size]
    return preview, data, blob[10]


def quantize256(rgba: np.ndarray) -> np.ndarray:
    """pvrtex's 256-colour quantisation (its preview is the result). pvrtex stops at 1024x1024: a bigger image gets
    the palette of a nearest-neighbour reduced copy, every pixel then takes its nearest palette colour."""
    h, w = rgba.shape[:2]
    small = rgba
    if w > 1024 or h > 1024:
        k = max(w, h) / 1024
        small = np.array(Image.fromarray(rgba, 'RGBA').resize((max(8, int(w / k)), max(8, int(h / k))), Image.NEAREST))
    sh, sw = small.shape[:2]
    canvas = np.pad(small, ((0, pot(sh) - sh), (0, pot(sw) - sw), (0, 0)), mode='edge')
    preview, _, _ = run_pvrtex(np.ascontiguousarray(canvas), ['-f', 'pal8bpp'])
    if small is rgba:
        return canonical(preview[:h, :w])
    pal = np.unique(canonical(preview[:sh, :sw]).reshape(-1, 4), axis=0).astype(np.int32)
    src = rgba.reshape(-1, 4).astype(np.int32)
    best = np.empty(len(src), np.int64)
    for i in range(0, len(src), 16384):
        best[i:i + 16384] = ((src[i:i + 16384, None, :] - pal[None, :, :]) ** 2).sum(-1).argmin(1)
    return canonical(pal[best].astype(np.uint8).reshape(rgba.shape))


def vq_pages(rgba: np.ndarray, fmt: int):
    """pvrtex VQ of every page: (page data list, the decoded image)"""
    h, w = rgba.shape[:2]
    name = {FMT_565: 'rgb565', FMT_1555: 'argb1555', FMT_4444: 'argb4444'}[fmt]
    datas, decoded = [], np.empty_like(rgba)
    for x0, y0, pw, ph, tw, th in pages_for(w, h):
        page = crop_page(rgba, x0, y0, pw, ph, tw, th)
        preview, data, cb = run_pvrtex(np.ascontiguousarray(page), ['-f', name, '-c', '256'])
        if cb != 255:   # a short codebook sits at the end of the 2 KB one: put back the unused entries' room
            data = b'\0' * ((255 - cb) * 8) + data
        assert len(data) == 2048 + tw * th // 4, (len(data), tw, th)
        datas.append(data)
        decoded[y0:y0 + ph, x0:x0 + pw] = preview[:ph, :pw]
    return datas, canonical(decoded)


def choose(rgba: np.ndarray, allow_vq: bool = True, log=None):
    """(kind, detail): ('pal', (colors, index image)) or ('16', fmt) or ('vq', (fmt, page datas))"""
    rgba = canonical(rgba)
    exact = palettize(rgba)
    if exact is not None:
        return 'pal', exact, 99.0
    fmt = alpha_kind(rgba)
    ref = psnr(rgba, from16(to16(rgba, fmt), fmt))
    h, w = rgba.shape[:2]
    if allow_vq and w >= 64 and h >= 64:
        datas, dec = vq_pages(rgba, fmt)
        q = psnr(rgba, dec)
        if log:
            log(f'VQ {q:.1f} dB vs 16-bit {ref:.1f} dB')
        if q >= ref - TOLERANCE_DB:
            return 'vq', (fmt, datas), q
    quant = quantize256(rgba)
    q = psnr(rgba, quant)
    if log:
        log(f'256 colours {q:.1f} dB vs 16-bit {ref:.1f} dB')
    if q >= ref - TOLERANCE_DB:
        pal = palettize(quant)
        if pal is not None:
            return 'pal', pal, q
    return '16', fmt, ref


def bake(rgba: np.ndarray, meta: bytes = b'', allow_vq: bool = True, log=None) -> tuple[bytes, str, int]:
    """(block, description, VRAM bytes)"""
    rgba = canonical(np.ascontiguousarray(rgba, dtype=np.uint8))
    h, w = rgba.shape[:2]
    kind, detail, q = choose(rgba, allow_vq, log)
    fmt16 = alpha_kind(rgba)
    pal = b''
    ncolors = 0
    page_fmt, page_data = [], []
    geo = pages_for(w, h)
    if kind == 'pal':
        colors, index = detail
        ncolors = len(colors)
        pal = argb8888(colors).tobytes()
        pf = FMT_PAL4 if ncolors <= 16 else FMT_PAL8
        for x0, y0, pw, ph, tw, th in geo:
            t = twiddle(crop_page(index, x0, y0, pw, ph, tw, th))
            if pf == FMT_PAL4:
                t = (t[0::2] | (t[1::2] << 4)).astype(np.uint8)
            page_fmt.append(pf)
            page_data.append(t.tobytes())
        desc = f'pal{4 if pf == FMT_PAL4 else 8} {ncolors}c'
    elif kind == 'vq':
        f, datas = detail
        page_fmt = [f | VQ] * len(geo)
        page_data = datas
        desc = f'VQ{("1555", "565", "4444")[f]} {q:.1f}dB'
    else:
        f = detail
        v = to16(rgba, f)
        for x0, y0, pw, ph, tw, th in geo:
            page_fmt.append(f)
            page_data.append(twiddle(crop_page(v, x0, y0, pw, ph, tw, th)).astype('<u2').tobytes())
        desc = f'{("1555", "565", "4444")[f]} {q:.1f}dB'
    head_len = 32 + 32 * len(geo)
    pal_off = head_len
    meta_off = pal_off + len(pad32(pal))
    data_off = meta_off + len(pad32(meta))
    table = b''
    body = b''
    vram = 0
    for (x0, y0, pw, ph, tw, th), pf, d in zip(geo, page_fmt, page_data):
        d = pad32(d)
        table += struct.pack('<6HIII8x', x0, y0, pw, ph, tw, th, pf, data_off + len(body), len(d))
        body += d
        vram += len(d)
    head = b'PVT1' + struct.pack('<HHHHHHIIII', w, h, len(geo), ncolors, fmt16, 0, meta_off, len(meta), pal_off, 0)
    block = head + table + pad32(pal) + pad32(meta) + body
    assert len(head) == 32 and len(block) % 32 == 0
    return block, desc, vram


def load_srgb(path) -> tuple[int, np.ndarray, bytes]:
    """a texprep dump: (kind 1 sprite / 2 cblock, RGBA, meta)"""
    with open(path, 'rb') as f:
        d = f.read()
    assert d[:4] == b'SRGB'
    kind, w, h, mlen = struct.unpack_from('<IIII', d, 4)
    meta = d[20:20 + mlen]
    px = np.frombuffer(d, np.uint8, w * h * 4, 20 + mlen).reshape(h, w, 4)
    return kind, px, meta


def relayout_cblock(px: np.ndarray, meta: bytes) -> tuple[np.ndarray, bytes]:
    """a cblock's tiles re-laid in the sheet width that needs the least texture memory (gfx.c reads sheet_cols)"""
    frames, cols, rows, tw, th, ntiles, sheet_cols, _ = struct.unpack_from('<8H', meta)
    tiles = [px[(t // sheet_cols) * th:(t // sheet_cols + 1) * th, (t % sheet_cols) * tw:(t % sheet_cols + 1) * tw]
             for t in range(ntiles)]
    best = None
    for w in (8 << i for i in range(8)):
        if w < tw:
            continue
        c = w // tw
        h = -(-ntiles // c) * th
        cost = vram_bytes(w, h, 8) + len(pages_for(w, h)) * 4096   # fewer pages: fewer header switches between tiles
        if best is None or cost < best[0]:
            best = (cost, c)
    c = best[1]
    sheet = np.zeros((-(-ntiles // c) * th, c * tw, 4), np.uint8)
    for t, tile in enumerate(tiles):
        sheet[(t // c) * th:(t // c + 1) * th, (t % c) * tw:(t % c + 1) * tw] = tile
    return sheet, struct.pack('<8H', frames, cols, rows, tw, th, ntiles, c, 0) + meta[16:]
