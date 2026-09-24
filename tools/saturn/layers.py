"""The Saturn's layer planner (plan 4.2 / 4.3): a level's tile layers as VDP2 scroll planes.

A level has more tile layers (level 1: 11, with 8 scroll rates) than VDP2 has normal planes (NBG0-3). The plan for a
level (PLANS below) puts each plane together from "bands" of cell rows; a band scrolls at one rate and holds the
layers composited into it:
  * layers of the band's rate, exactly;
  * a layer of another rate, merged at the band's rate (e.g. the far range, 0.2 on PC, at the mountains' 0.3);
  * "anchored": each piece of a layer (a run of non-empty columns: a car) is shifted so that it sits exactly where the
    PC draws it when it is in the middle of the screen, and drifts by (rate - band rate) x its screen travel
    elsewhere (the cars of Cars MidBG, 0.9 on PC, in the 0.875 plane: under 10 px);
  * "flat": a static layer behind a moving band, as one colour per row (the sky's horizon behind the mountains), from
    a given pixel row down;
the plane's line scroll (NBG0 / NBG1 only) gives each band its own rate. A "backdrop" is a static strip of a layer
drawn by VDP1 as a palette sprite at a priority under the planes (level 1: the planet's lower edge, which the moving
mountain band would otherwise cut). The tile layers no plane takes are left to the core's VDP1 drawing (level 1:
ForegroundStuff and ForegroundStuf2, over the sprites).

The planes are 8x8 cells of 4bpp, deduplicated across flips, each with one of the level's 16-colour palettes (a cell
with more than 15 colours is quantised); names are VDP2 2-word pattern names. Everything stays in video memory for the
whole stage but the names, which the runtime (src/platform/saturn/vdp2_planes.c) streams as the camera moves.

Output ("SPL1" block, big-endian but the magic; the runtime reads it in place):
  0 "SPL1"  4 u16 nplanes, nbands   8 u16 npal, nbackdrops   12 u32 ncells   16 u32 bands_off, names_off
  24 u32 cellpal_off, backdrops_off   32 u32 the level layers the planes draw (bit n: layer n)
  planes (16 bytes each, at 36): u8 nbg, prio, first_band, nbands; u32 first_cell, ncells; u8 line_scroll, 0, 0, 0
  bands (32 bytes each): u8 plane, row0, row1 (cell rows [row0, row1)), flags (1 static); i32 rate (16.16);
         u32 cols (the band's width in cells), wrap (repeat every `wrap` columns, 0 none); u32 chunk_first, nchunks;
         u32 0, 0
  palettes (npal x 16 x u16 RGB555, entry 0 unused) after the bands
  cell palettes (cellpal_off): u8 per cell (all planes' cells in order), padded to 4
  backdrops (backdrops_off, 12 bytes each): u32 texture id (a SAT1 texture in the same pack, 8bpp only), i16 x, y,
         u8 priority, 0, 0, 0
  chunk table (u32 offset from the block, u32 stored length | 0x80000000 when LZ4) at names_off, then the chunks:
         CHUNK_COLS columns each, a column = (row1 - row0) u16 names, top to bottom: VF << 15 | HF << 14 | cell (the
         plane's own numbering, 0 = the plane's empty cell); its palette is the cell's
The cells go in blocks of their own, CELL_CHUNK cells (32 KB) each, read one at a time into video memory and released
(one big block may find no room once the menus have fragmented the heap): 4bpp, two pixels a byte, the left one in the
high nibble; a plane's cells follow the previous plane's.
The level block itself is rewritten without the maps the planes took (slim_level: the core keeps 600 KB less).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import struct

import numpy as np

import levl
import pckwrite

CHUNK_COLS = 16
PALETTES = 64              # of CRAM's 128 16-colour palettes (the other 64: VDP1's 8bpp sprites; 96 left them too few in a level)
CELL_CHUNK = 1024          # 32 KB: the renderer's staging buffer
SCREEN_W = 352             # the widest mode (anchoring uses the middle of the screen)

# Plane layout per level: nbg, priority (sprites are 6), bands (rows in 8 px cells, rate, layers), and backdrops.
# A layer entry is a name, or (name, 'anchored') or (name, 'flat', first pixel row).
PLANS = {
    0x12DAD1A7: dict(
        planes=[
            dict(nbg=0, prio=2, bands=[
                dict(rows=(0, 10), rate=0.0, layers=['SkyBG']),
                dict(rows=(10, 22), rate=0.3, layers=[('SkyBG', 'flat', 96), 'FarMountains', 'Mountains']),
            ]),
            dict(nbg=1, prio=3, bands=[dict(rows=(0, 32), rate=0.4, layers=['NearMountains'])]),
            dict(nbg=2, prio=4, bands=[dict(rows=(0, 32), rate=0.875, layers=['MidBG', ('Cars MidBG', 'anchored')])]),
            dict(nbg=3, prio=5, bands=[dict(rows=(0, 32), rate=1.0, layers=['Playfield', 'Platforms', 'Cars'])]),
        ],
        # the sky rows 72-95 behind the mountain band: the planet's lower edge stays where it is (from 72: the band
        # scrolls up by 0.3 x the camera's 16-19 px on the Saturn's 224 lines, its lines then start above row 80)
        backdrops=[dict(layer='SkyBG', rows=(72, 96), prio=1)],
    ),
}
BACKDROP_XOR = 0x42440000   # a backdrop's texture id: level id ^ this ^ its number


def rgb555(px: np.ndarray) -> np.ndarray:
    r = (px[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (px[..., 1].astype(np.uint32) * 31 + 127) // 255
    b = (px[..., 2].astype(np.uint32) * 31 + 127) // 255
    return np.where(px[..., 3] >= 128, 0x8000 | b << 10 | g << 5 | r, 0).astype(np.uint16)


def layer_image(ly: levl.Layer, bank: levl.Bank, rows: int) -> np.ndarray:
    """a whole tile layer as RGB555 (0 transparent), its map's full width, `rows` pixel rows from the top"""
    tw, th = bank.tw, bank.th
    w = ly.used_w if ly.extra == 1 else ly.w
    out = np.zeros((rows, w * tw), np.uint16)
    tiles: dict[int, np.ndarray] = {}
    ncells = len(bank.cells)
    for cy in range(min(ly.h, -(-rows // th))):
        for cx in range(w):
            v = int(ly.cells[cy, cx]) & 0x7FFFFFFF
            if not v:
                continue
            t = int(bank.cells[(v - 1) % ncells])
            if t == 0xFFFF:
                continue
            if t not in tiles:
                tiles[t] = rgb555(bank.tile(t))
            tile = tiles[t][:rows - cy * th]
            dst = out[cy * th:cy * th + tile.shape[0], cx * tw:cx * tw + tw]
            np.copyto(dst, tile, where=tile != 0)
    return out


def paste(dst: np.ndarray, src: np.ndarray, x: int) -> None:
    """src over dst at column x (clipped)"""
    x0, x1 = max(0, x), min(dst.shape[1], x + src.shape[1])
    if x1 <= x0:
        return
    s = src[:, x0 - x:x1 - x]
    d = dst[:, x0:x1]
    np.copyto(d, s, where=s != 0)


def pieces(img: np.ndarray) -> list[tuple[int, int]]:
    """runs of non-empty pixel columns (x0, x1)"""
    used = img.any(0)
    out, x = [], 0
    while x < len(used):
        if used[x]:
            x0 = x
            while x < len(used) and used[x]:
                x += 1
            out.append((x0, x))
        else:
            x += 1
    return out


@dataclass
class Band:
    plane: int
    row0: int
    row1: int
    rate: float
    static: bool
    img: np.ndarray            # RGB555, (row1 - row0) * 8 rows
    wrap: int = 0              # columns
    names: np.ndarray | None = None   # (rows, cols) u32
    notes: list[str] = field(default_factory=list)


def compose_band(L: levl.Level, bank_of, plane: int, spec: dict) -> Band:
    r0, r1 = spec['rows']
    rate = spec['rate']
    y0, y1 = r0 * 8, r1 * 8
    by_name = {ly.name: ly for ly in L.layers}
    imgs = []
    for entry in spec['layers']:
        name, mode, *arg = (entry, '') if isinstance(entry, str) else entry
        ly = by_name[name]
        full = layer_image(ly, bank_of(ly.cblock), y1)[y0:y1]
        imgs.append((ly, mode, full, arg))
    static = rate == 0.0
    if static:
        width = max(img.shape[1] for _, _, img, _ in imgs)
    else:   # a moving band reaches as far as its widest layer at the band's rate
        width = max(img.shape[1] for ly, mode, img, _ in imgs if mode != 'flat')
    width = -(-width // 8) * 8
    out = np.zeros((y1 - y0, width), np.uint16)
    band = Band(plane, r0, r1, rate, static, out)
    for ly, mode, img, arg in imgs:
        if mode == 'flat':   # one colour per row: what the PC shows of it on screen (x < 320), its most common colour
            vis = img[:, :320]
            for y in range(max(0, arg[0] - y0) if arg else 0, vis.shape[0]):
                row = vis[y][vis[y] != 0]
                if len(row):
                    vals, cnt = np.unique(row, return_counts=True)
                    out[y, :] = np.where(out[y, :] == 0, vals[cnt.argmax()], out[y, :])
            band.notes.append(f'{ly.name}: flat (one colour a row) from y {arg[0] if arg else y0}')
        elif mode == 'anchored':
            shifts = []
            for x0, x1 in pieces(img):
                cam = max(0.0, ((x0 + x1) / 2 - SCREEN_W / 2) / ly.parallax)
                dx = int(round(-(ly.parallax - rate) * cam))
                paste(out, img[:, x0:x1], x0 + dx)
                travel = SCREEN_W / ly.parallax / 2   # camera distance from the middle of the screen to its edge
                shifts.append(abs((ly.parallax - rate) * travel))
            band.notes.append(f'{ly.name}: anchored, {len(shifts)} pieces, at most {max(shifts, default=0):.1f} px off at the screen edges')
        else:
            if abs(ly.parallax - rate) > 1e-4 and not static:
                band.notes.append(f'{ly.name}: merged at {rate} (PC {ly.parallax})')
            paste(out, img, 0)
    if static:
        band.wrap = 0
    return band


# ---------------------------------------------------------------- cells and palettes
def quantise_cell(c: np.ndarray, k: int = 15) -> np.ndarray:
    vals, inv, cnt = np.unique(c[c != 0], return_inverse=True, return_counts=True)
    x = np.stack([vals & 31, vals >> 5 & 31, vals >> 10 & 31], -1).astype(float)
    idx = np.argsort(-cnt)[:k]
    cent = x[idx].copy()
    for _ in range(20):
        a = ((x[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
        for j in range(len(cent)):
            m = a == j
            if m.any():
                cent[j] = np.average(x[m], axis=0, weights=cnt[m])
    q = np.clip(np.round(cent), 0, 31).astype(np.uint32)
    qv = (0x8000 | q[:, 2] << 10 | q[:, 1] << 5 | q[:, 0]).astype(np.uint16)
    a = ((x[:, None] - q[None].astype(float)) ** 2).sum(-1).argmin(1)
    out = c.copy()
    out[c != 0] = qv[a][inv]
    return out


class Cells:
    """one plane's cells: deduplicated across flips; cell 0 is the empty one"""

    def __init__(self) -> None:
        self.keys: dict[bytes, tuple[int, bool, bool]] = {}
        self.pix: list[np.ndarray] = [np.zeros((8, 8), np.uint16)]
        self.keys[self.pix[0].tobytes()] = (0, False, False)
        self.quantised = 0

    def add(self, c: np.ndarray) -> tuple[int, bool, bool]:
        """(cell, hflip, vflip) that draws c"""
        if not c.any():
            return 0, False, False
        k = c.tobytes()
        hit = self.keys.get(k)
        if hit:
            return hit
        if len(np.unique(c[c != 0])) > 15:
            c = quantise_cell(c)
            self.quantised += 1
            k2 = c.tobytes()
            if k2 in self.keys:
                self.keys[k] = self.keys[k2]
                return self.keys[k]
        i = len(self.pix)
        self.pix.append(c)
        for hf in (False, True):
            for vf in (False, True):
                v = c[:, ::-1] if hf else c
                v = v[::-1] if vf else v
                self.keys.setdefault(v.tobytes(), (i, hf, vf))
        self.keys[k] = (i, False, False)
        return self.keys[k]


def to_rgb(v) -> np.ndarray:
    v = np.asarray(v, np.int64)
    return np.stack([v & 31, v >> 5 & 31, v >> 10 & 31], -1).astype(np.float64)


def pack_palettes(hists: list[tuple[np.ndarray, np.ndarray]], k: int, iters: int = 16) -> tuple[list[list[int]], np.ndarray, float]:
    """cells (their colours and pixel counts) -> k palettes of <= 15 colours, each cell's palette, and the PSNR (5-bit
    scale) of drawing every cell with its palette's nearest colours. Lossy clustering (plan 4.5, palette packing): the
    exact packing of level 1 needs 455 palettes, CRAM holds 128. Cells go to the palette that draws them best, a
    palette is refitted to its cells' colours (weighted k-means, snapped to colours that exist), and again."""
    U = np.unique(np.concatenate([c for c, _ in hists]))
    X = to_rgb(U)
    col_of = {int(u): i for i, u in enumerate(U)}
    rows = np.concatenate([np.full(len(c), i) for i, (c, _) in enumerate(hists)]).astype(np.int64)
    cols = np.array([col_of[int(x)] for c, _ in hists for x in c], np.int64)
    w = np.concatenate([n for _, n in hists]).astype(np.float64)
    n = len(hists)
    order = np.argsort(-np.bincount(rows, w, n))
    pals: list[list[int]] = []
    for i in order:   # start from the heaviest cells' own colours, a few colours apart from each other
        s = [int(x) for x in hists[i][0]][:15]
        if all(len(set(s) - set(p)) > 2 for p in pals):
            pals.append(s)
        if len(pals) == k:
            break
    a = np.zeros(n, np.int64)
    err = 0.0
    for it in range(iters + 1):
        cost = np.zeros((len(U), len(pals)))
        for j, p in enumerate(pals):
            cost[:, j] = ((X[:, None] - to_rgb(p)[None]) ** 2).sum(-1).min(1)
        cc = np.zeros((n, len(pals)))
        np.add.at(cc, rows, w[:, None] * cost[cols])
        a = cc.argmin(1)
        err = float(cc[np.arange(n), a].sum())
        if it == iters:
            break
        for j in range(len(pals)):
            m = a[rows] == j
            if not m.any():
                continue
            hist = np.bincount(cols[m], w[m], len(U))
            nz = np.nonzero(hist)[0]
            if len(nz) <= 15:
                pals[j] = [int(U[i]) for i in nz]
                continue
            x, ww = X[nz], hist[nz]
            cent = x[np.argsort(-ww)[:15]].copy()
            for _ in range(15):
                asg = ((x[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
                for q in range(15):
                    mm = asg == q
                    if mm.any():
                        cent[q] = np.average(x[mm], axis=0, weights=ww[mm])
            snap = set()
            for q in range(15):
                mm = asg == q
                if mm.any():
                    cand = nz[mm]
                    snap.add(int(U[cand[((X[cand] - cent[q]) ** 2).sum(-1).argmin()]]))
            pals[j] = sorted(snap)
    mse = err / w.sum() / 3
    return pals, a, 10 * np.log10(31 ** 2 / mse) if mse > 0 else 99.0


def cell_indices(c: np.ndarray, pal: list[int]) -> np.ndarray:
    """a cell's pixels as indices into its palette (the nearest colour; 0 transparent)"""
    idx = np.zeros((8, 8), np.uint8)
    m = c != 0
    if m.any():
        d = ((to_rgb(c[m])[:, None] - to_rgb(pal)[None]) ** 2).sum(-1)
        idx[m] = d.argmin(1) + 1
    return idx


def cell_bytes(idx: np.ndarray) -> bytes:
    return ((idx[:, 0::2] << 4) | idx[:, 1::2]).astype(np.uint8).tobytes()


# ---------------------------------------------------------------- the whole level
@dataclass
class Result:
    spl: bytes
    spc: list[bytes]
    slim: bytes
    report: list[str]
    bands: list[Band]
    cells: list[Cells]
    palettes: list[list[int]]
    cell_pal: list[list[int]]
    taken: set[str]
    idx: list[list[np.ndarray]]                            # each plane's cells as palette indices
    backdrops: list[tuple[int, int, int, int, np.ndarray]]  # texture id, x, y, priority, RGBA


def rgba(v: np.ndarray) -> np.ndarray:
    """RGB555 (0 transparent) -> RGBA8"""
    c = to_rgb(v) * 255 / 31
    return np.concatenate([np.round(c), np.where(v != 0, 255, 0)[..., None]], -1).astype(np.uint8)


def pack_chunk(raw: bytes) -> tuple[bytes, int]:
    z = pckwrite.lz4_compress(raw)
    return (z, 0x80000000) if len(z) < len(raw) else (raw, 0)


def bake(level_path: Path, srgb_dir: Path) -> Result:
    L = levl.load(level_path)
    plan = PLANS[L.id]
    banks: dict[int, levl.Bank] = {}

    def bank_of(i: int) -> levl.Bank:
        if i not in banks:
            banks[i] = levl.load_bank(srgb_dir / f'{i:08X}.srgb')
        return banks[i]

    report: list[str] = []
    bands: list[Band] = []
    planes_cells: list[Cells] = []
    taken: set[str] = set()
    for pi, p in enumerate(plan['planes']):
        cells = Cells()
        planes_cells.append(cells)
        for spec in p['bands']:
            b = compose_band(L, bank_of, pi, spec)
            for e in spec['layers']:
                taken.add(e if isinstance(e, str) else e[0])
            rows, cols = b.img.shape[0] // 8, b.img.shape[1] // 8
            refs = np.zeros((rows, cols, 3), np.int32)
            for r in range(rows):
                for c in range(cols):
                    refs[r, c] = cells.add(b.img[r * 8:r * 8 + 8, c * 8:c * 8 + 8])
            b.names = refs
            bands.append(b)
            report += [f'NBG{p["nbg"]} band rows {b.row0}-{b.row1} rate {b.rate}: {cols} columns'] + [f'  {n}' for n in b.notes]
        report.append(f'NBG{p["nbg"]}: {len(cells.pix)} cells ({len(cells.pix) * 32 // 1024} KB), {cells.quantised} quantised to 15 colours')
    if max(len(c.pix) for c in planes_cells) > 1 << 14:
        raise ValueError('a plane has more cells than a name addresses (16384)')
    # palettes over every plane's cells (the empty cells take palette 0)
    hists, owners = [], []
    for pi, cells in enumerate(planes_cells):
        for ci, c in enumerate(cells.pix[1:], 1):
            u, cnt = np.unique(c[c != 0], return_counts=True)
            hists.append((u, cnt))
            owners.append((pi, ci))
    palettes, of, db = pack_palettes(hists, PALETTES)
    cell_pal = [[0] * len(c.pix) for c in planes_cells]
    for (pi, ci), pal in zip(owners, of):
        cell_pal[pi][ci] = int(pal)
    ncells = sum(len(c.pix) for c in planes_cells)
    report.append(f'palettes: {len(palettes)} of 16 colours ({len(palettes) * 16} CRAM entries), {db:.1f} dB (5-bit scale); '
                  f'cells {ncells} ({ncells * 32 // 1024} KB)')

    # SPC1: the cells, drawn with their palettes
    cell_data = bytearray()
    idx_of: list[list[np.ndarray]] = []
    for pi, cells in enumerate(planes_cells):
        idx_of.append([])
        for ci, c in enumerate(cells.pix):
            idx = cell_indices(c, palettes[cell_pal[pi][ci]])
            idx_of[-1].append(idx)
            cell_data += cell_bytes(idx)
    spc = [bytes(cell_data[i:i + CELL_CHUNK * 32]) for i in range(0, len(cell_data), CELL_CHUNK * 32)]

    # backdrops: static strips of a layer, as the PC shows them (x from 0)
    backdrops = []
    by_name = {ly.name: ly for ly in L.layers}
    for k, bd in enumerate(plan.get('backdrops', [])):
        ly = by_name[bd['layer']]
        y0, y1 = bd['rows']
        img = layer_image(ly, bank_of(ly.cblock), y1)[y0:y1, :SCREEN_W]
        backdrops.append((L.id ^ BACKDROP_XOR ^ k, 0, y0, bd['prio'], rgba(img)))
        report.append(f'backdrop {k}: {ly.name} rows {y0}-{y1}, {img.shape[1]} px wide, priority {bd["prio"]} (VDP1)')

    # SPL1: planes, bands, palettes, cell palettes, backdrops, names
    head_len, plane_len, band_len = 36, 16, 32
    bands_off = head_len + plane_len * len(plan['planes'])
    pal_off = bands_off + band_len * len(bands)
    cellpal_off = pal_off + len(palettes) * 32
    backdrops_off = cellpal_off + -(-ncells // 4) * 4
    names_off = backdrops_off + 12 * len(backdrops)
    nchunks = [-(-b.names.shape[1] // CHUNK_COLS) for b in bands]
    data_at = names_off + 8 * sum(nchunks)
    body = bytearray()
    table = bytearray()
    for b in bands:
        rows, cols = b.names.shape[:2]
        for c0 in range(0, cols, CHUNK_COLS):
            col = b.names[:, c0:c0 + CHUNK_COLS]
            words = [int(col[r, c, 2]) << 15 | int(col[r, c, 1]) << 14 | int(col[r, c, 0])
                     for c in range(col.shape[1]) for r in range(rows)]
            stored, flag = pack_chunk(struct.pack(f'>{len(words)}H', *words))
            table += struct.pack('>II', data_at + len(body), len(stored) | flag)
            body += stored + b'\0' * (-len(stored) % 4)
    mask = sum(1 << i for i, ly in enumerate(L.layers) if ly.is_tilemap and ly.name in taken)
    spl = bytearray(b'SPL1' + struct.pack('>HHHHIIIIII', len(plan['planes']), len(bands), len(palettes), len(backdrops),
                                          ncells, bands_off, names_off, cellpal_off, backdrops_off, mask))
    first_cell, first_band = 0, 0
    for pi, p in enumerate(plan['planes']):
        nb = len(p['bands'])
        spl += struct.pack('>BBBBIIBBBB', p['nbg'], p['prio'], first_band, nb, first_cell, len(planes_cells[pi].pix),
                           1 if nb > 1 else 0, 0, 0, 0)
        first_cell += len(planes_cells[pi].pix)
        first_band += nb
    chunk_first = 0
    for b, n in zip(bands, nchunks):
        spl += struct.pack('>BBBBiIIIIII', b.plane, b.row0, b.row1, 1 if b.static else 0, int(round(b.rate * 65536)),
                           b.names.shape[1], b.wrap, chunk_first, n, 0, 0)
        chunk_first += n
    for pal in palettes:
        spl += struct.pack('>16H', 0, *pal, *([0] * (15 - len(pal))))
    cp = bytes(v for c in cell_pal for v in c)
    spl += cp + b'\0' * (-len(cp) % 4)
    for tid, x, y, prio, _ in backdrops:
        spl += struct.pack('>IhhBBBB', tid, x, y, prio, 0, 0, 0)
    assert len(spl) == names_off
    spl += table + body
    report.append(f'names: {len(body) // 1024} KB stored; cells: {len(spc)} blocks of up to {CELL_CHUNK * 32 // 1024} KB')
    return Result(bytes(spl), spc, slim_level(level_path.read_bytes(), taken), report, bands, planes_cells,
                  palettes, cell_pal, taken, idx_of, backdrops)


def vdp1_tiles(level_path: Path, taken: set[str]) -> dict[int, set[int]]:
    """the tiles of the layers left to VDP1, per tile bank that a plane layer uses too: the bank's texture on the disc
    can be cut down to them (level 1: ForegroundStuff's 27 tiles of the playfield's 2020)"""
    L = levl.load(level_path)
    plane_banks = {ly.cblock for ly in L.layers if ly.is_tilemap and ly.name in taken}
    out: dict[int, set[int]] = {}
    for ly in L.layers:
        if not ly.is_tilemap or ly.name in taken or ly.cblock not in plane_banks:
            continue
        out.setdefault(ly.cblock, set()).update(int(v) & 0x7FFFFFFF for v in np.unique(ly.cells) if int(v) & 0x7FFFFFFF)
    return out


def slim_level(d: bytes, taken: set[str]) -> bytes:
    """the level block without the tile maps of the layers the planes draw (w = h = 0, no tile bank)"""
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    npacks, nobjs = u32(8), u32(12)
    p = 0x10 + npacks * 12 + nobjs * 128
    nlayers = u32(p); p += 4
    flags, names = p, p + 192
    layers = []
    for i in range(nlayers):
        layers.append((d[names + i * 16:names + i * 16 + 15].split(b'\0')[0].decode('latin1'),
                       struct.unpack_from('<i', d, flags + i * 4)[0] != 0))
    p += 192 + 15 * 16 + 16 + 4
    cols, rows = u32(p + 8), u32(p + 12)
    p += 24 + cols * rows * 2
    out = bytearray(d[:p])
    for name, is_map in layers:
        if not is_map or p + 12 > len(d):
            continue
        w, h = u32(p), u32(p + 4)
        n = 12 + w * h * 4
        out += struct.pack('<III', 0, 0, 0) if name in taken else d[p:p + n]
        p += n
    out += d[p:]
    return bytes(out)


# ---------------------------------------------------------------- preview: what the planes show against the PC
def band_pixels(res: Result, b: Band) -> np.ndarray:
    """the band as the VDP2 draws it: names -> cells -> palettes"""
    if getattr(b, '_pixels', None) is not None:
        return b._pixels
    rows, cols = b.names.shape[:2]
    out = np.zeros((rows * 8, cols * 8), np.uint16)
    pals = [np.array([0] + p + [0] * (15 - len(p)), np.uint16) for p in res.palettes]
    for r in range(rows):
        for c in range(cols):
            i, hf, vf = (int(v) for v in b.names[r, c])
            if not i:
                continue
            px = pals[res.cell_pal[b.plane][i]][res.idx[b.plane][i]]
            px = px[:, ::-1] if hf else px
            px = px[::-1] if vf else px
            out[r * 8:r * 8 + 8, c * 8:c * 8 + 8] = px
    b._pixels = out
    return out
def preview(res: Result, level_path: Path, srgb_dir: Path, cams: list[float], sw: int = 320, sh: int = 224) -> np.ndarray:
    L = levl.load(level_path)
    banks: dict[int, levl.Bank] = {}
    rows_out = []
    for cam in cams:
        pc = np.zeros((sh, sw, 4), np.uint8)
        pc[..., 3] = 255
        for ly in L.layers:
            if ly.is_tilemap and ly.name in res.taken:
                if ly.cblock not in banks:
                    banks[ly.cblock] = levl.load_bank(srgb_dir / f'{ly.cblock:08X}.srgb')
                levl.render_layer(ly, banks[ly.cblock], levl.layer_offset(ly, cam), sw, sh, pc)
        sat = np.zeros((sh, sw), np.uint16)
        for _, x, y, _, img in res.backdrops:   # under the planes
            part = rgb555(img)[:, :sw][:max(0, sh - y)]
            np.copyto(sat[y:y + part.shape[0], x:x + part.shape[1]], part, where=part != 0)
        for pi in range(len(res.cells)):   # back to front: plan order
            for b in res.bands:
                if b.plane != pi:
                    continue
                off = 0 if b.static else int(max(0.0, cam * b.rate))
                y0, y1 = b.row0 * 8, min(b.row1 * 8, sh)
                if y0 >= sh:
                    continue
                src = band_pixels(res, b)[:y1 - y0]
                xs = np.arange(sw) + off
                ok = xs < src.shape[1]
                part = np.zeros((y1 - y0, sw), np.uint16)
                part[:, ok] = src[:, xs[ok]]
                np.copyto(sat[y0:y1], part, where=part != 0)
        v = sat.astype(np.uint32)
        rgb = np.stack([(v & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v >> 10 & 31) * 255 // 31], -1).astype(np.uint8)
        rows_out.append(np.concatenate([pc[..., :3], rgb], 1))
    return np.concatenate(rows_out, 0)
