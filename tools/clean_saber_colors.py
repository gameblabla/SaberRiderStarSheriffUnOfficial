#!/usr/bin/env python3
"""Give the complete Saber sheet the clean run's outline and clothing colors.

Usage: python3 tools/clean_saber_colors.py [sheet.png]
Idempotent; preserves all cell positions, silhouettes, and animation geometry.
Run this after rebuilding the legacy sheet. The idle/run extractors already
apply the same cleanup to their recovered video pixels.
"""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

from saber_art import clean_colors


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "assets/saber.png"
    image = np.array(Image.open(path).convert("RGBA"))
    original = image.copy()
    for y in range(0, image.shape[0], 64):
        for x in range(0, image.shape[1], 64):
            image[y:y + 64, x:x + 64] = clean_colors(image[y:y + 64, x:x + 64])
    assert np.array_equal(original[:, :, 3], image[:, :, 3])
    assert np.array_equal(clean_colors(image), image), "Color cleanup must be idempotent"
    Image.fromarray(image).save(path)
    changed = np.any(original[:, :, :3] != image[:, :, :3], axis=2).sum()
    print(f"Cleaned {changed} color pixels in {path}; alpha and sprite layout preserved")


if __name__ == "__main__":
    main()
