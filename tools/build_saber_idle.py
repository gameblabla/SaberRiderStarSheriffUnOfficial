#!/usr/bin/env python3
"""Graft the green-screen tweet's breathing and salute onto assets/saber.png.

Usage: python3 tools/build_saber_idle.py [video.mp4]
Requires ffmpeg, Pillow and NumPy. The clip is 10 fps, with native pixels
enlarged 4x. Decode every frame without frame-rate conversion, take the median
of each 4x4 block to suppress H.264 noise, key the green background, and snap
foreground colors using the idle clip's source palette, then map its separate
black/navy shades to the run palette. Keep the full 64x64 canvas:
cropping/recentering each pose would erase the breathing motion and foot anchor.
Left-facing idle cells mirror the native right footage. Matching stationary
regions of aim cells are restored from idle; other legacy cells are preserved.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

from saber_art import recover_colors, restore_standing_aim

ROOT = Path(__file__).resolve().parent.parent


def main():
    video = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "video.twimg.com_tweet_video_DhBgC6xWsAAgACs.mp4"
    sheet_path = ROOT / "assets/saber.png"
    original = Image.open(sheet_path).convert("RGBA").crop((0, 0, 512, 1216))
    sheet = Image.new("RGBA", (512, 27 * 64))
    sheet.paste(original, (0, 0))
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(video),
                        "-fps_mode", "passthrough", str(Path(tmp) / "%02d.png")], check=True)
        frames = sorted(Path(tmp).glob("*.png"))
        if len(frames) != 31:
            raise ValueError("Expected the supplied 31-frame, 256x256 tweet clip")
        recovered = []
        for path in frames:
            rgb = np.asarray(Image.open(path).convert("RGB"))
            if rgb.shape != (256, 256, 3):
                raise ValueError("Expected 4x enlarged 64x64 pixel art")
            rgb = np.median(rgb.reshape(64, 4, 64, 4, 3), axis=(1, 3))
            recovered.append(Image.fromarray(recover_colors(rgb, idle=True)))
        # Frames 1..12 breathe; 13..25 raise the hand, salute, then lower it.
        # The remaining tail is another hold of the resting pose.
        for first, indices in ((152, range(12)), (184, range(12, 25))):
            right_first = 168 if first == 152 else 200
            for offset, index in enumerate(indices):
                right = recovered[index]
                for cell, image in ((first + offset, right.transpose(Image.Transpose.FLIP_LEFT_RIGHT)),
                                    (right_first + offset, right)):
                    sheet.paste(image, ((cell % 8) * 64, (cell // 8) * 64))
    Image.fromarray(restore_standing_aim(sheet)).save(sheet_path)
    print(f"Wrote {sheet_path}: breathing L/R 152..163 / 168..179, salute 184..196 / 200..212")


if __name__ == "__main__":
    main()
