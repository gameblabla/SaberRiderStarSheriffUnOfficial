#!/usr/bin/env python3
"""Graft the green-screen tweet's breathing and salute onto assets/saber.png.

Usage: python3 tools/build_saber_idle.py [video.mp4]
Requires ffmpeg, Pillow and NumPy. The clip is 10 fps, with native pixels
enlarged 4x. Decode every frame without frame-rate conversion, take the median
of each 4x4 block to suppress H.264 noise, key the green background, and snap
foreground colors to Saber's existing palette. Keep the full 64x64 canvas:
cropping/recentering each pose would erase the breathing motion and foot anchor.
Cells 0..151 are preserved; left-facing cells mirror the native right footage.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def main():
    video = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "video.twimg.com_tweet_video_DhBgC6xWsAAgACs.mp4"
    sheet_path = ROOT / "assets/saber.png"
    original = Image.open(sheet_path).convert("RGBA").crop((0, 0, 512, 1216))
    pixels = np.asarray(original)
    palette = np.unique(pixels[pixels[:, :, 3] != 0, :3], axis=0).astype(float)
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
            green = (rgb[:, :, 1] > rgb[:, :, 0] * 1.35) & (rgb[:, :, 1] > rgb[:, :, 2] * 1.2)
            nearest = ((rgb[:, :, None, :] - palette) ** 2).sum(axis=3).argmin(axis=2)
            rgba = np.zeros((64, 64, 4), dtype=np.uint8)
            rgba[~green, :3] = palette[nearest[~green]]
            rgba[~green, 3] = 255
            recovered.append(Image.fromarray(rgba))
        # Frames 1..12 breathe; 13..25 raise the hand, salute, then lower it.
        # The remaining tail is another hold of the resting pose.
        for first, indices in ((152, range(12)), (184, range(12, 25))):
            right_first = 168 if first == 152 else 200
            for offset, index in enumerate(indices):
                right = recovered[index]
                for cell, image in ((first + offset, right.transpose(Image.Transpose.FLIP_LEFT_RIGHT)),
                                    (right_first + offset, right)):
                    sheet.paste(image, ((cell % 8) * 64, (cell // 8) * 64))
    sheet.save(sheet_path)
    print(f"Wrote {sheet_path}: breathing L/R 152..163 / 168..179, salute 184..196 / 200..212")


if __name__ == "__main__":
    main()
