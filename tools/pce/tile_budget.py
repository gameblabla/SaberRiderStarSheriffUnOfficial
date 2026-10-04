"""Keep every 33-column window of a platform background within the VRAM tile cache.

The renderer keeps 33 BAT columns resident and caches each distinct 8x8 character once, so a window
with more distinct characters than the cache holds leaves its rightmost column unloaded (garbage while
scrolling). When a stage is over budget, single-use characters in the crowded windows are dropped: the
cell is redrawn with the character (and one of the four palettes of its colour band) from the nearby
columns that matches the original pixels best. Cheapest first, and only while a window still gains.
"""
import numpy as np
from formats import vce_rgb

WINDOW = 33
REACH = 8                  # candidate replacements come from this many columns either side

def presence(grid):
    """(tiles, windows) bool: which 33-column windows contain each tile."""
    rows, cols = grid.shape
    n_windows = max(cols - WINDOW + 1, 1)
    p = np.zeros((int(grid.max()) + 1, n_windows), bool)
    for x in range(cols):
        lo, hi = max(x - WINDOW + 1, 0), min(x, n_windows - 1)
        p[np.unique(grid[:, x]), lo:hi + 1] = True
    return p

def limit_tiles(cells, indices, groups, palettes, cols, cap):
    """cells: (n, 8, 8, 4) source RGBA; indices: (n, 8, 8) palette indices; groups: (n,) palette numbers
    (band * 4 + k); all row-major. Returns (indices, groups, merged cells), copies."""
    indices = np.array(indices, np.uint8); groups = np.array(groups, np.uint8)
    n = len(indices); rows = n // cols
    flat = indices.reshape(n, 64)
    uniq, inverse = np.unique(flat, axis=0, return_inverse=True)
    inverse = inverse.reshape(-1).astype(np.int64)
    if presence(inverse.reshape(rows, cols)).sum(0).max() <= cap: return indices, groups, 0
    colours = [vce_rgb(p).astype(np.int32) for p in palettes]
    colours[15][15] = 4096        # BG colour 255 is the text font's white at runtime: never a candidate for a baked cell
    source = np.where(cells[..., 3:4] >= 128, cells[..., :3], 0).reshape(n, 64, 3).astype(np.int32)
    ar = np.arange(64)
    merged = 0
    for _ in range(40):
        grid = inverse.reshape(rows, cols)
        p = presence(grid)
        excess = p.sum(0) - cap
        if excess.max() <= 0: break
        weight = np.bincount(inverse, minlength=len(uniq))
        crowded = (p & (excess > 0)).any(1) & (weight == 1)
        cell_of = np.full(len(uniq), -1)
        cell_of[inverse] = np.arange(n)
        moves = []
        for a in np.nonzero(crowded)[0]:
            i = cell_of[a]; x = i % cols
            near = np.unique(grid[:, max(x - REACH, 0):x + REACH + 1]); near = near[near != a]
            if not len(near): continue
            band = int(groups[i]) // 4
            best = None
            for pal in range(band * 4, band * 4 + 4):
                cost = ((colours[pal][None] - source[i][:, None]) ** 2).sum(-1)      # (64 pixels, 16 indices)
                err = cost[ar[None], uniq[near]].sum(1)
                k = int(err.argmin())
                if best is None or err[k] < best[0]: best = (int(err[k]), int(a), int(near[k]), pal, int(i))
            moves.append(best)
        moves.sort()
        removed, targets, progress = set(), set(), False
        for err, a, b, pal, i in moves:
            if a in targets or b in removed: continue
            if not (p[a] & p[b] & (excess > 0)).any(): continue
            inverse[i] = b; groups[i] = pal
            p[b] |= p[a]; p[a] = False
            excess = p.sum(0) - cap
            removed.add(a); targets.add(b); merged += 1; progress = True
            if excess.max() <= 0: break
        if not progress: raise ValueError('tile budget cannot be met by merging')
    else:
        raise ValueError('tile budget not reached')
    return uniq[inverse].reshape(-1, 8, 8), groups, merged
