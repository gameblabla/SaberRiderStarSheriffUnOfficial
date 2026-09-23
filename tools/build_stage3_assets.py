#!/usr/bin/env python3
"""Build the reviewed 1x Stage 3 scenery from the cleaned masters.

The masters remain untouched.  Reduction uses a box average followed by a
non-dithered palette quantization; alpha is made binary for solid props.  The
manifest is intentionally machine-readable so a review run can record the
source hashes and native dimensions.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
OUT = ASSETS / "stage3" / "native"

TARGETS = {
    "stage3_night_sky.png": ("stage3_night_sky.png", (720, 240), False, 64),
    "stage3_red_moon.png": ("stage3_red_moon.png", (96, 96), True, 48),
    "stage3_static_mesa.png": ("stage3_static_mesa.png", (160, 154), True, 48),
    "stage3_static_cactus.png": ("stage3_static_cactus.png", (64, 94), True, 48),
    "stage3_static_scrub.png": ("stage3_static_scrub.png", (64, 46), True, 48),
    "stage3_static_fence.png": ("stage3_static_fence.png", (112, 79), True, 48),
    "stage3_static_wreck.png": ("stage3_static_wreck.png", (128, 75), True, 64),
    "stage3_static_pad.png": ("stage3_static_pad.png", (112, 88), True, 64),
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def make_native(src: Path, dst: Path, size: tuple[int, int], binary_alpha: bool, colors: int) -> None:
    image = Image.open(src).convert("RGBA")
    image = image.resize(size, Image.Resampling.BOX)
    if binary_alpha:
        alpha = image.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
        rgb = image.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGB")
        image = Image.merge("RGBA", (*rgb.split(), alpha))
    else:
        image = image.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGBA")
    dst.parent.mkdir(parents=True, exist_ok=True)
    image.save(dst, optimize=True)


def build() -> dict:
    manifest = {"version": 1, "masters": {}, "native": {}}
    for output, (source_name, size, binary_alpha, colors) in TARGETS.items():
        src = ASSETS / source_name
        dst = OUT / output
        if not src.is_file():
            raise SystemExit(f"missing Stage 3 master: {src}")
        make_native(src, dst, size, binary_alpha, colors)
        manifest["masters"][source_name] = sha256(src)
        manifest["native"][output] = {
            "size": list(size), "colors": colors, "binary_alpha": binary_alpha,
            "source": source_name, "sha256": sha256(dst),
        }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def check() -> None:
    manifest_path = OUT / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit("native assets are missing; run without --check first")
    manifest = json.loads(manifest_path.read_text())
    for output, (source_name, size, binary_alpha, _colors) in TARGETS.items():
        src = ASSETS / source_name
        dst = OUT / output
        if manifest["masters"].get(source_name) != sha256(src):
            raise SystemExit(f"master changed: {source_name}; rebuild and review")
        if not dst.is_file():
            raise SystemExit(f"native asset missing: {dst}")
        image = Image.open(dst).convert("RGBA")
        if image.size != size:
            raise SystemExit(f"wrong dimensions for {output}: {image.size}, expected {size}")
        pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
        if binary_alpha and any(a not in (0, 255) for *_, a in pixels):
            raise SystemExit(f"non-binary alpha in {output}")
        expected = manifest["native"].get(output, {}).get("sha256")
        if expected != sha256(dst):
            raise SystemExit(f"native output changed: {output}; rebuild and review")
    print(f"stage3 assets OK: {len(TARGETS)} native images")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        check()
    else:
        build()
        print(f"wrote {len(TARGETS)} native Stage 3 images to {OUT}")


if __name__ == "__main__":
    main()
