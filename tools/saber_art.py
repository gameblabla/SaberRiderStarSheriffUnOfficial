"""Fixed palettes for Saber's reconstructed poses and native video frames.

The idle clip's black and navy are brighter than the run clip's. Classify each
clip with its own source colors before mapping to the shared output colors;
otherwise idle's navy shadow gets merged into the lighter cloth/metal shade.
Legacy neutral gray is black even inside the silhouette. Guessing its material
from neighbors filled shadows with navy, red and silver and erased detail.
"""

import numpy as np

SOURCE_PALETTE = np.array([
    (8, 8, 25), (16, 16, 66), (33, 33, 41), (41, 41, 66),
    (49, 58, 107), (74, 82, 90), (90, 132, 173), (107, 107, 115),
    (165, 165, 165), (222, 222, 222), (247, 247, 247),
    (107, 25, 41), (173, 66, 66), (165, 8, 49),
    (230, 148, 82), (239, 181, 82), (25, 165, 132),
], dtype=float)

# Median RGB centers measured in the supplied idle clip's native 4x4 blocks.
# Keep its navy shadow separate from both the lighter navy and dark metal.
IDLE_SOURCE_PALETTE = SOURCE_PALETTE.copy()
IDLE_SOURCE_PALETTE[:4] = [
    (31, 30, 40), (33, 31, 63), (51, 51, 60), (41, 37, 73),
]
RUN_SOURCE_PALETTE = SOURCE_PALETTE.copy()
RUN_SOURCE_PALETTE[:4] = [
    (7, 6, 23), (20, 16, 50), (51, 51, 60), (28, 22, 65),
]
# Both clips retain the same three cloth shades. Previously the run's two navy
# shades merged, while idle's highlight snapped to the much brighter (41,41,66).
VIDEO_OUTPUT_PALETTE = SOURCE_PALETTE.copy()
VIDEO_OUTPUT_PALETTE[:4] = [
    (8, 8, 25), (20, 16, 50), (41, 41, 66), (28, 22, 65),
]

RUN_CELLS = frozenset(first + i for first in (64, 80, 88, 104) for i in range(6))
LEGACY_DARK = np.array([(8, 8, 25), (16, 16, 66), (41, 41, 66), (49, 58, 107)], dtype=np.uint8)


def recover_colors(rgb, *, idle=False):
    """Snap decoded native RGB blocks without merging the idle's dark ramp."""
    rgb = np.asarray(rgb, dtype=float)
    palette = IDLE_SOURCE_PALETTE if idle else RUN_SOURCE_PALETTE
    green = (rgb[:, :, 1] > rgb[:, :, 0] * 1.35) & (rgb[:, :, 1] > rgb[:, :, 2] * 1.2)
    nearest = ((rgb[:, :, None, :] - palette) ** 2).sum(axis=3).argmin(axis=2)
    rgba = np.zeros((*rgb.shape[:2], 4), dtype=np.uint8)
    rgba[~green, :3] = VIDEO_OUTPUT_PALETTE[nearest[~green]]
    rgba[~green, 3] = 255
    return clean_colors(rgba)


def clean_colors(cell):
    """Normalize legacy black and the gun's teal/cyan without changing alpha."""
    source = np.asarray(cell, dtype=np.uint8)
    out = source.copy()
    neutral = (source[:, :, 3] > 0) & np.all(source[:, :, :3] == (33, 33, 41), axis=2)
    out[neutral, :3] = (8, 8, 25)
    teal = (source[:, :, 3] > 0) & np.all(source[:, :, :3] == (25, 165, 132), axis=2)
    # Teal belongs exclusively to the reconstructed gun. Its neighboring cyan
    # pixels are gun highlights; distant blue armor highlights remain untouched.
    gun = teal.copy()
    for _ in range(3):
        gun = _expand(gun)
    cyan = gun & (source[:, :, 3] > 0) & np.all(source[:, :, :3] == (90, 132, 173), axis=2)
    out[teal, :3] = (74, 74, 82)
    out[cyan, :3] = (107, 107, 115)
    return out


def _expand(mask):
    padded = np.pad(mask, 1)
    height, width = mask.shape
    return np.logical_or.reduce([padded[y:y + height, x:x + width]
                                 for y in range(3) for x in range(3)])


def _components(mask):
    """Four-connected patches: diagonal checkerboard pixels stay separate."""
    seen = np.zeros_like(mask)
    height, width = mask.shape
    for y, x in np.argwhere(mask):
        if seen[y, x]:
            continue
        stack = [(y, x)]
        seen[y, x] = True
        patch = []
        while stack:
            yy, xx = stack.pop()
            patch.append((yy, xx))
            for ny, nx in ((yy - 1, xx), (yy + 1, xx), (yy, xx - 1), (yy, xx + 1)):
                if 0 <= ny < height and 0 <= nx < width and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        yield patch


def clean_legacy_pose(cell):
    """Remove small reconstruction shade islands, preserving larger folds.

Only the four dark shades participate. Red, silver, skin and alpha never vote
as replacement colors. Native video poses must not go through this filter.
Use whole patches and converge to a fixed point so repeated cleanup is stable.
"""
    source = clean_colors(cell)
    for _ in range(64):
        labels = np.full(source.shape[:2], -1, dtype=np.int8)
        for label, color in enumerate(LEGACY_DARK):
            labels[(source[:, :, 3] > 0) & np.all(source[:, :, :3] == color, axis=2)] = label
        out = source.copy()
        for label in range(len(LEGACY_DARK)):
            for patch in _components(labels == label):
                if len(patch) > 3:
                    continue
                mask = np.zeros(labels.shape, dtype=bool)
                yy, xx = np.array(patch).T
                mask[yy, xx] = True
                neighbors = labels[_expand(mask) & ~mask]
                dark = neighbors[neighbors >= 0]
                if len(dark) < 4 or len(dark) < len(neighbors) * 0.6:
                    continue
                counts = np.bincount(dark, minlength=len(LEGACY_DARK))
                winner = counts.argmax()
                if counts[winner] >= 4 and counts[winner] >= len(dark) * 0.6:
                    out[yy, xx, :3] = LEGACY_DARK[winner]
        if np.array_equal(out, source):
            return out
        source = out
    raise ValueError("Legacy shade cleanup did not converge")


def restore_standing_aim(sheet):
    """Restore matching stationary regions from native idle, without mirroring the chest.

Left-facing aim exposes the same front armor and resting arm as left idle.
Only their shared opaque core takes native RGB; the aiming arm, gun and head
retain their pose. Both sides use the native standing feet at the same anchor,
so changing aim angle or firing cannot change the foot shading or silhouette.
"""
    out = np.array(sheet, dtype=np.uint8, copy=True)
    if out.shape[1] != 512 or out.shape[0] < 22 * 64:
        return out  # legacy-only sheet: native reference cells not appended yet

    def cell(index):
        y, x = divmod(index, 8)
        return out[y * 64:(y + 1) * 64, x * 64:(x + 1) * 64]

    for first, reference in ((16, 152), (40, 168)):
        native = cell(reference).copy()
        if not native[54:, :, 3].any():
            continue
        for index in range(first, first + 6):
            aim = cell(index)
            if first == 16:
                core = np.zeros((64, 64), dtype=bool)
                core[27:39, 29:45] = True
                core &= (aim[:, :, 3] > 0) & (native[:, :, 3] > 0)
                aim[core, :3] = native[core, :3]
            aim[54:] = native[54:]
    return out


def clean_sheet(sheet):
    """Clean reconstructed cells, then restore stationary aim regions."""
    out = np.array(sheet, dtype=np.uint8, copy=True)
    columns = out.shape[1] // 64
    # Native restoration can expose a small shade island at the patch boundary.
    # Converge the combined operation, keeping restored colors fixed each pass.
    for _ in range(64):
        previous = out.copy()
        for y in range(0, out.shape[0], 64):
            for x in range(0, out.shape[1], 64):
                index = y // 64 * columns + x // 64
                cleanup = clean_legacy_pose if index < 152 and index not in RUN_CELLS else clean_colors
                out[y:y + 64, x:x + 64] = cleanup(out[y:y + 64, x:x + 64])
        out = restore_standing_aim(out)
        if np.array_equal(out, previous):
            return out
    raise ValueError("Sheet cleanup did not converge")
