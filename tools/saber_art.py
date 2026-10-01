"""Shared source palette and color cleanup for all of Saber's sprite poses.

Keep the source palette fixed: deriving it from the destination PNG made video
recovery depend on whichever edits were already present in that file. Neutral
gray (33,33,41) is common in the old reconstruction/idle footage, whereas the
clean run uses (8,8,25) outlines and navy cloth. Resolve that gray by material
before snapping every pose to the same final color treatment.
"""
from collections import Counter

import numpy as np

SOURCE_PALETTE = np.array([
    (8, 8, 25), (16, 16, 66), (33, 33, 41), (41, 41, 66),
    (49, 58, 107), (74, 82, 90), (90, 132, 173), (107, 107, 115),
    (165, 165, 165), (222, 222, 222), (247, 247, 247),
    (107, 25, 41), (173, 66, 66), (165, 8, 49),
    (230, 148, 82), (239, 181, 82), (25, 165, 132),
], dtype=float)

NAVY = {(41, 41, 66), (16, 16, 66), (49, 58, 107)}
SILVER = {(74, 82, 90), (107, 107, 115), (165, 165, 165),
          (222, 222, 222), (247, 247, 247), (90, 132, 173)}
RED = {(107, 25, 41), (173, 66, 66), (165, 8, 49)}


def clean_colors(cell):
    """Resolve dull outlines/interior gray flecks without changing alpha or pose.

Read neighbors from the original cell so traversal order cannot affect color.
Only the neutral gray is replaced: the run's actual navy shading, metal
highlights, red accents, face, gun and black outline pixels remain intact.
Three neighbors in the same material identify armor/red/cloth speckles;
otherwise an interior dark pixel gets the native run's main navy cloth shade.
"""
    source = np.asarray(cell, dtype=np.uint8)
    out = source.copy()
    mask = source[:, :, 3] > 0
    height, width = mask.shape
    neutral = mask & np.all(source[:, :, :3] == (33, 33, 41), axis=2)
    for y, x in np.argwhere(neutral):
        edge = any(not (0 <= y + dy < height and 0 <= x + dx < width and mask[y + dy, x + dx])
                   for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)))
        if edge:
            color = (8, 8, 25)
        else:
            nearby = [tuple(source[yy, xx, :3])
                      for yy in range(max(0, y - 1), min(height, y + 2))
                      for xx in range(max(0, x - 1), min(width, x + 2))
                      if (yy, xx) != (y, x) and mask[yy, xx]]
            votes = [[v for v in nearby if v in group] for group in (NAVY, SILVER, RED)]
            pick = max(range(3), key=lambda i: len(votes[i]))
            color = Counter(votes[pick]).most_common(1)[0][0] if len(votes[pick]) >= 3 else (41, 41, 66)
        out[y, x, :3] = color
    return out
