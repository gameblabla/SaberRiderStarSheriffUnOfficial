#!/usr/bin/env python3
"""Screenshot the player in a set of scripted poses and tile the crops (hero review).

pose_sheet.py out.png [hero] [start_x]   e.g. pose_sheet.py /tmp/april.png 2 1700
Runs the game headless (SDL offscreen driver) once per pose with SABER_SCRIPT, reads the player's position from the
SABER_TRACE line at the screenshot step and crops a 96x96 window around it, 4x nearest."""
import os, re, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)
POSES = [("idle", "400:"), ("alert", "20:S,120:"), ("run", "100:R"), ("run+shoot", "100:RS"), ("run+aim up", "100:RSU"),
         ("aim L", "100:AL"), ("aim UL", "100:ALU"), ("aim U", "100:AU"), ("aim UR", "100:ARU"), ("aim R", "100:AR"),
         ("aim DR", "100:ARD"), ("aim D", "100:AD"), ("aim DL", "100:ALD"), ("shoot", "100:S"),
         ("crouch", "100:D"), ("crouch+shoot", "100:DS"), ("slide", "20:D,80:DJ"), ("jump", "30:,60:J"),
         ("fall", "30:,8:J,52:"), ("fall+shoot", "30:,8:J,52:S"), ("fall+aim up", "30:,8:J,52:SU"), ("fall+aim down", "30:,8:J,52:SD"),
         ("walk L", "100:L"), ("death", "200:")]

def shoot(name, script, hero, start, steps=88):
    bmp = f"/tmp/pose_{os.getpid()}.bmp"
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy", SABER_HERO=str(hero), SABER_START=str(start),
               SABER_SCRIPT=script, SABER_SHOT=f"{bmp},-1,{steps}", SABER_WINDOW="426x240", SABER_TRACE="1")
    if name == "death": env["SABER_KILL"] = "60"
    r = subprocess.run([os.path.join(GAME, "build", "saber_rider"), os.path.join(GAME, "..", "SaberRider", "data")],
                       env=env, cwd=GAME, capture_output=True, text=True, timeout=60)
    lines = [l for l in r.stderr.splitlines() if l.startswith("cam=")]
    m = re.search(r"cam=(-?\d+).*pos=(-?[\d.]+),(-?[\d.]+)", lines[-1]) if lines else None
    im = Image.open(bmp).convert("RGB"); os.unlink(bmp)
    if not m: return im.crop((165, 72, 261, 168))
    cam, px, py = float(m.group(1)), float(m.group(2)), float(m.group(3))
    sx, sy = int(px - cam), int(py)          # cam_y is 0 in level 1
    return im.crop((sx - 48, sy - 56, sx + 48, sy + 40))

def main():
    out = sys.argv[1]; hero = int(sys.argv[2]) if len(sys.argv) > 2 else 2; start = int(sys.argv[3]) if len(sys.argv) > 3 else 1700
    cols = 8
    sheet = Image.new("RGB", (96 * cols, 96 * ((len(POSES) + cols - 1) // cols)))
    for i, (name, script) in enumerate(POSES):
        steps = 88 if name != "death" else 150
        sheet.paste(shoot(name, script, hero, start, steps), ((i % cols) * 96, (i // cols) * 96))
        print(name, flush=True)
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(out)

if __name__ == "__main__":
    main()
