"""Joint palette fitting for tile-based backgrounds (the PC Engine's 16 BG palettes of 15 colours, one chosen per 8x8 character).

The old baker grouped the cells by their mean colour into four vertical bands and quantised each group with median cut. A cell whose
mean sat near a group boundary got a palette that fitted its neighbour badly, which showed as rectangles of a different tint, and the
sky (a smooth gradient) had a handful of flat bands. Here the palettes and the cell assignment are optimised together, as k-means
does for points: every cell takes the palette that reproduces its pixels with the least error, every palette is refitted to the
cells that took it (a weighted k-means over the 512 colours the hardware can show), and the two steps repeat.
"""
import numpy as np
from formats import vce_colors, vce_rgb

def _lattice():
    return vce_rgb(np.arange(512)).astype(np.float32)

LATTICE = _lattice()

def fit_colors(hist, count=15, iterations=12, rng=None):
    """Weighted k-means over the 512 hardware colours: `hist` (512,) pixel counts -> up to `count` colour codes."""
    used = np.nonzero(hist)[0]
    if len(used) <= count:
        return used.astype(np.int64)
    pts = LATTICE[used]
    w = np.sqrt(hist[used].astype(np.float32))     # a rare colour keeps a say (a flat area does not swamp a highlight)
    # farthest-point start: the first centre is the heaviest colour, each next the point farthest from the centres so far
    centres = [int(w.argmax())]
    dist = ((pts - pts[centres[0]]) ** 2).sum(1)
    for _ in range(count - 1):
        k = int((dist * w).argmax())
        centres.append(k)
        dist = np.minimum(dist, ((pts - pts[k]) ** 2).sum(1))
    c = pts[centres].copy()
    for _ in range(iterations):
        d = ((pts[:, None, :] - c[None]) ** 2).sum(-1)
        a = d.argmin(1)
        for k in range(len(c)):
            m = a == k
            if m.any():
                c[k] = (pts[m] * w[m, None]).sum(0) / w[m].sum()
    # snap each centre to the nearest hardware colour (each distinct, empty centres dropped)
    codes = []
    for k in range(len(c)):
        near = ((LATTICE - c[k]) ** 2).sum(1)
        for code in np.argsort(near)[:8]:
            if int(code) not in codes:
                codes.append(int(code)); break
    return np.asarray(codes, np.int64)

def _words(pal):
    """16 palette words: entry 0 is the transparent slot, the unused tail repeats the first colour (never a stray black)."""
    out = np.zeros(16, np.uint16)
    if len(pal):
        out[1:] = int(pal[0]) & 0x1ff
        out[1:len(pal) + 1] = [(int(v) & 0x1ff) for v in pal]
    return out

def error_table(palette_codes):
    """(512,) squared distance from every hardware colour to the nearest colour of the palette."""
    pc = LATTICE[palette_codes]
    d = ((LATTICE[:, None, :] - pc[None]) ** 2).sum(-1)
    return d.min(1)

def fit_palettes(cells, npal, ncolors=15, iterations=8, groups=None, fixed=(), verbose=True):
    """cells: (n, 8, 8, 4) RGBA. Returns (palettes (npal, 16) '<u2' with entry 0 unused, groups (n,) uint8).
    `fixed`: palette numbers that are never assigned (they stay empty)."""
    n = len(cells)
    rgb = cells[..., :3].reshape(n, 64, 3)
    opaque = (cells[..., 3].reshape(n, 64) >= 128)
    codes = vce_colors(rgb).astype(np.int64)
    usable = [p for p in range(npal) if p not in fixed]
    if groups is None:
        # start: k-means on the cells' mean colour (a few rounds), so the first palettes are spatially sensible
        mean = np.where(opaque[..., None], rgb, 0).sum(1) / np.maximum(opaque.sum(1), 1)[:, None]
        centres = mean[np.linspace(0, n - 1, len(usable), dtype=int)].astype(np.float32)
        for _ in range(6):
            g = ((mean[:, None, :] - centres[None]) ** 2).sum(-1).argmin(1)
            for k in range(len(usable)):
                if (g == k).any():
                    centres[k] = mean[g == k].mean(0)
        groups = np.asarray(usable, np.uint8)[g]
    groups = np.asarray(groups, np.uint8).copy()
    palettes = np.zeros((npal, 16), np.uint16)
    tables = np.zeros((npal, 512), np.float32) + 1e9
    flat_codes = np.where(opaque, codes, -1)
    for it in range(iterations):
        for p in usable:
            sel = groups == p
            if not sel.any():
                continue
            c = flat_codes[sel].ravel(); c = c[c >= 0]
            hist = np.bincount(c, minlength=512)
            pal = fit_colors(hist, ncolors)
            palettes[p, :] = _words(pal)
            tables[p] = error_table(pal) if len(pal) else 1e9
        # cost[n, p]: error of cell n under palette p
        cost = np.zeros((n, len(usable)), np.float32)
        safe = np.where(opaque, codes, 0)
        for j, p in enumerate(usable):
            e = tables[p][safe] * opaque
            cost[:, j] = e.sum(1)
        new = np.asarray(usable, np.uint8)[cost.argmin(1)]
        changed = int((new != groups).sum())
        groups = new
        if verbose:
            print(f'    palette fit {it}: {changed} cells moved, error {cost.min(1).sum() / max(opaque.sum(), 1):.2f} a pixel', flush=True)
        if changed == 0:
            break
    # final palettes for the final assignment
    for p in usable:
        sel = groups == p
        if sel.any():
            c = flat_codes[sel].ravel(); c = c[c >= 0]
            pal = fit_colors(np.bincount(c, minlength=512), ncolors)
            palettes[p, :] = _words(pal)
    return palettes, groups

def index_cells(cells, palettes, groups):
    """(n, 8, 8) palette indices of every cell under its own palette (0 where transparent)."""
    n = len(cells)
    rgb = cells[..., :3].reshape(n, 64, 3)
    opaque = cells[..., 3].reshape(n, 64) >= 128
    codes = vce_colors(rgb).astype(np.int64)
    out = np.zeros((n, 64), np.uint8)
    luts = {}
    for p in np.unique(groups):
        pal = palettes[p]
        pc = LATTICE[(pal[1:] & 0x1ff).astype(np.int64)]
        d = ((LATTICE[:, None, :] - pc[None]) ** 2).sum(-1)
        luts[int(p)] = d.argmin(1).astype(np.uint8) + 1
    for p, lut in luts.items():
        sel = groups == p
        out[sel] = np.where(opaque[sel], lut[codes[sel]], 0)
    return out.reshape(n, 8, 8)
