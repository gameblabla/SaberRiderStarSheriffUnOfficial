#!/usr/bin/env python3
"""Verify Saber's idle/aim distinction, timed salute and input cancellation on SDL PC.

Run after `make`: python3 tools/check_saber_idle.py [output-directory]
Uses the PC renderer with SDL's offscreen driver; saves screenshots and traces.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/saber-idle-pc")
OUT.mkdir(parents=True, exist_ok=True)


def run(name, script, steps):
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               SABER_ASSETS=str(ROOT / "assets"), SABER_HERO="0", SABER_START="100",
               SABER_SCRIPT=script, SABER_SHOT=f"{OUT / name}.bmp,-1,{steps}",
               SABER_WINDOW="426x240", SABER_TRACE="1", SABER_FAST="1")
    env.pop("SABER_BORED", None)  # exercise the real ten-second delay
    result = subprocess.run([str(ROOT / "saber_rider"), "SaberRider/data", "--level", "1"],
                            cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
    (OUT / f"{name}.log").write_text(result.stderr)
    assert result.returncode == 0, result.stderr[-1000:]
    rows = [tuple(map(int, m)) for m in re.findall(r"st=(\d+).*?anim=(\d+) frame=(\d+) ov=(\d+)", result.stderr)]
    assert rows, f"No player trace in {name}"
    return rows


for name, script, idle, bored, first in (
        ("right", "1500:", 2, 54, 200),
        ("left", "350:,1:AL,1200:", 1, 53, 184)):
    rows = run(name, script, 1500)
    start = next(i for i, row in enumerate(rows) if row[1] == idle)
    salute = next(i for i, row in enumerate(rows) if row[1] == bored)
    assert 599 <= salute - start <= 601, (name, start, salute)
    salute_rows = [row for row in rows if row[1] == bored]
    assert set(row[2] for row in salute_rows) == set(range(first, first + 13))
    assert 77 <= len(salute_rows) <= 79, (name, len(salute_rows))
    assert sum(row[2] == first + 12 for row in salute_rows) >= 5
    assert rows[-1][1] == idle and all(row[3] == 0 for row in salute_rows)
    print(f"{name}: ten-second delay, all 13 salute frames at 10 fps, return to breathing")

for name, keys, expected in (("move", "R", 37), ("aim", "AR", 7),
                              ("shoot", "S", 15), ("jump", "J", 47), ("crouch", "D", 41)):
    rows = run(name, f"1205:,50:{keys},300:", 1210)
    assert rows[-6][1] == 54, (name, rows[-6])
    assert rows[-1][1] == expected, (name, rows[-1])
    print(f"{name}: cancels salute immediately, animation {expected}")

rows = run("aim-held", "1500:AR", 1500)
assert all(row[1] != 54 for row in rows) and rows[-1][1] == 7
rows = run("repeat", "2300:", 2300)
assert sum(row[1] == 54 and (i == 0 or rows[i - 1][1] != 54) for i, row in enumerate(rows)) == 2
print("Held aim stays gun-ready; continued idle repeats the salute after another ten seconds")
run("breathing", "800:", 800)
run("salute-right", "1300:", 1206)
run("salute-left", "350:,1:AL,1000:", 1281)
print(f"PC screenshots and traces: {OUT}")
