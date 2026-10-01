#!/usr/bin/env python3
"""Recover Saber's native six-frame run in both directions from the tweet clip.

Usage: python3 tools/build_saber_run.py [video.mp4]
Requires ffmpeg, NumPy and Pillow. The 256x512 video contains two 64x64
canvases enlarged 4x (right above left), at 10 fps. Frame 7 duplicates frame 6.
Preserve the canvas/foot anchor and the 0/1/2-pixel body bob; split at the hip
and remove that bob only from torso cells, since character_draw adds it back.
Only run cells 64..69, 80..85, 88..93, 104..109 are replaced.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

from saber_art import recover_colors

ROOT = Path(__file__).resolve().parent.parent
BOB = (0, 1, 2, 0, 1, 2)  # must match heroes.c


def main():
    video = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "video.twimg.com_tweet_video_DVAP6WHVAAAZKD3.mp4"
    sheet_path = ROOT / "assets/saber.png"
    sheet = Image.open(sheet_path).convert("RGBA")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-fps_mode", "passthrough",
                        str(Path(tmp) / "%02d.png")], check=True)
        frames = sorted(Path(tmp).glob("*.png"))
        if len(frames) != 7:
            raise ValueError("Expected the supplied seven-frame running clip")
        for index, path in enumerate(frames[:6]):
            rgb = np.asarray(Image.open(path).convert("RGB"))
            if rgb.shape != (512, 256, 3):
                raise ValueError("Expected two 4x enlarged 64x64 canvases")
            rgb = np.median(rgb.reshape(128, 4, 64, 4, 3), axis=(1, 3))
            rgba = recover_colors(rgb)
            for side, torso_first, legs_first in ((0, 88, 104), (1, 64, 80)):
                full = rgba[side * 64:(side + 1) * 64]
                bob = BOB[index]
                cut = 38 + bob
                legs = full.copy()
                legs[:cut] = 0
                torso = np.zeros_like(full)
                torso[:cut - bob] = full[bob:cut]
                for cell, art in ((torso_first + index, torso), (legs_first + index, legs)):
                    sheet.paste(Image.fromarray(art), (cell % 8 * 64, cell // 8 * 64))
                # The runtime must recombine the exact recovered clip pixels.
                rebuilt = legs.copy()
                upper = torso[:cut - bob]
                rebuilt[bob:cut] = upper
                assert np.array_equal(full, rebuilt)
    sheet.save(sheet_path)
    print(f"Wrote {sheet_path}: six native run poses per direction; torso bob {BOB}")


if __name__ == "__main__":
    main()
