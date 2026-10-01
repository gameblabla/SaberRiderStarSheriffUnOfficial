#!/usr/bin/env python3
"""Normalize Saber's dark colors, grey gun, and reconstructed shade speckles.

Usage: python3 tools/clean_saber_colors.py [sheet.png]
Idempotent; preserves cell positions and restores consistent native aim feet.
Run this after rebuilding the legacy sheet. The idle/run extractors already
normalize their colors; native poses are excluded from the speckle filter.
The old neighbor-based cleanup is lossy: regenerate affected cells from their
source before applying this version; it cannot recover already erased shadows.
"""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

from saber_art import clean_sheet


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "assets/saber.png"
    image = np.array(Image.open(path).convert("RGBA"))
    original = image.copy()
    image = clean_sheet(image)
    assert np.array_equal(clean_sheet(image), image), "Color cleanup must be idempotent"
    allowed = np.zeros(image.shape[:2], dtype=bool)
    for index in (*range(16, 22), *range(40, 46)):
        y, x = divmod(index, 8)
        allowed[y * 64 + 54:(y + 1) * 64, x * 64:(x + 1) * 64] = True
    assert np.array_equal(original[~allowed, 3], image[~allowed, 3]), "Alpha changed outside aim feet"
    Image.fromarray(image).save(path)
    changed = np.any(original[:, :, :3] != image[:, :, :3], axis=2).sum()
    foot_alpha = np.count_nonzero(original[:, :, 3] != image[:, :, 3])
    print(f"Cleaned {changed} color pixels in {path}; {foot_alpha} foot outline pixels aligned to native idle")


if __name__ == "__main__":
    main()
