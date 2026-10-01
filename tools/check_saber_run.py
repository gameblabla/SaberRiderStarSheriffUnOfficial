#!/usr/bin/env python3
"""Check the recovered six-frame run and shooting overlays in the SDL PC game.

Run after make: python3 tools/check_saber_run.py [output-directory]
Requires Pillow for the gameplay contact sheet. Saves screenshots and traces.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = (Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/saber-run-pc")).resolve()
OUT.mkdir(parents=True, exist_ok=True)


def run(case):
    name, keys, steps, body, overlay = case
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               SABER_ASSETS=str(ROOT / "assets"), SABER_HERO="0", SABER_START="100",
               SABER_SCRIPT=f"350:,80:{keys},30:", SABER_SHOT=f"{OUT / name}.bmp,-1,{steps}",
               SABER_WINDOW="426x240", SABER_TRACE="1", SABER_FAST="1")
    result = subprocess.run([str(ROOT / "saber_rider"), "SaberRider/data", "--level", "1"],
                            cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
    (OUT / f"{name}.log").write_text(result.stderr)
    assert result.returncode == 0, result.stderr[-1000:]
    lines = [line for line in result.stderr.splitlines() if line.startswith("cam=")]
    rows = [tuple(map(int, match)) for match in re.findall(
        r"st=(\d+).*?anim=(\d+) frame=(\d+) ov=(\d+) ovf=(\d+)", "\n".join(lines))]
    walking = [row for row in rows if row[0] == 1]
    assert walking and all(row[1] == body and row[3] == overlay for row in walking), name
    if overlay in (38, 39):
        body_first, torso_first = (80, 64) if body == 36 else (104, 88)
        assert all(row[2] - body_first == row[4] - torso_first for row in walking), name
    if steps == 390:
        assert len(set(row[2] for row in walking)) == 6, name
        changes = [i for i in range(1, len(walking)) if walking[i][2] != walking[i - 1][2]]
        assert all(b - a == 6 for a, b in zip(changes, changes[1:])), (name, changes)
    return name, lines[-1]


cases = []
for direction, keys, body, overlay in (("right", "R", 37, 39), ("left", "L", 36, 38)):
    for frame in range(6):
        cases.append((f"{direction}-{frame}", keys, 355 + 6 * frame, body, overlay))
    cases.append((f"{direction}-shoot", keys + "S", 390, body, overlay))
    cases.append((f"{direction}-up", keys + "SU", 390, body, 29 if keys == "L" else 32))
    cases.append((f"{direction}-down", keys + "SD", 390, body, 30 if keys == "L" else 33))

with ThreadPoolExecutor(max_workers=3) as executor:
    results = dict(executor.map(run, cases))

preview = Image.new("RGB", (6 * 240, 2 * 210), (30, 30, 30))
draw = ImageDraw.Draw(preview)
for row, direction in enumerate(("right", "left")):
    for frame in range(6):
        name = f"{direction}-{frame}"
        cam, x, y = map(float, re.search(r"cam=(-?\d+).*pos=([\d.]+),([\d.]+)", results[name]).groups())
        x = int(x - cam)
        y = int(y)
        image = Image.open(OUT / f"{name}.bmp").convert("RGB")
        image = image.crop((x - 40, y - 40, x + 40, y + 24)).resize((240, 192), Image.Resampling.NEAREST)
        preview.paste(image, (frame * 240, row * 210))
        draw.text((frame * 240 + 4, row * 210 + 194), name, fill="white")
preview.save(OUT / "preview.png")
print("PC checks passed: six poses at 10 fps in both directions, synchronized run/level-shot torsos, diagonal shots")
print(f"Screenshots and traces: {OUT}")
