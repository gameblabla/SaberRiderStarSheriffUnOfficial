#!/usr/bin/env python3
"""Check actual sprite joins and barrel pixels against the game's pose manifest.

make -f Makefile.headless check-heroes [FIXED=1]
python3 tools/check_hero_poses.py build/headless/hero_poses_test --sheet /tmp/heroes.png
Requires Pillow and the original demo packs in SaberRider/data.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--sheet')
    parser.add_argument('--assets', default='assets')
    args = parser.parse_args()
    assets = Path(args.assets).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        drawlog = Path(tmp) / 'draw.log'
        run = subprocess.run([args.binary], capture_output=True, text=True,
                             env={**os.environ, 'SABER_ASSETS': str(assets),
                                  'SABER_DRAWLOG': str(drawlog), 'SABER_DRAWLOG_EVERY': '1'})
        draw_frames = []
        draws = []
        if drawlog.exists():
            for line in drawlog.read_text().splitlines():
                fields = line.split()
                if fields[0] == 'F':
                    draw_frames.append(draws)
                    draws = []
                elif fields[0] in ('tex', 'rot') and fields[1] in ('53414245', '79260A58'):
                    draws.append((tuple(map(float, fields[3:7])), tuple(map(float, fields[8:12]))))
    if run.returncode:
        raise RuntimeError(run.stderr)
    sheets = [Image.open(assets / f'{name}.png').convert('RGBA') for name in ('saber', 'april')]
    examples = {}
    checked = 0
    for line in run.stdout.splitlines():
        h, side, pose, aim, shoot, frame, overlay, ox, oy, mx, my, tick = map(int, line.split())
        def cell(index):
            x, y = index % 8 * 64, index // 8 * 64
            return sheets[h].crop((x, y, x + 64, y + 64))
        legs = cell(frame)
        torso = cell(overlay) if overlay >= 0 else legs
        tx, ty = (ox, oy) if overlay >= 0 else (0, 0)
        label = (h, side, pose, aim, shoot, frame, overlay)
        # Inspect the actual render calls, rather than assuming each cell is
        # drawn whole. The top of a run legs cell must never emit stray art;
        # cropping its source must preserve the feet's destination anchor.
        rendered = draw_frames[checked]
        assert len(rendered) == (2 if overlay >= 0 else 1), (label, rendered)
        source, destination = rendered[0]
        first_row = round(source[1]) - frame // 8 * 64
        assert source[0] == frame % 8 * 64 and source[2] == 64, (label, source)
        assert first_row + source[3] == 64 and destination[3] == source[3], (label, rendered)
        assert destination[1] == -32 + first_row, (label, rendered)
        if h == 0 and pose == 1:
            assert first_row >= 38, (label, 'run legs include unrelated upper art', source)
        else:
            assert first_row == 0, (label, 'unexpected crop', source)
        if first_row:
            legs.paste((0, 0, 0, 0), (0, 0, 64, first_row))
        if aim & 1:
            dx = -1 if aim in (1, 7) else 1
            dy = -1 if aim in (1, 3) else 1
            # Find the outermost barrel pixel in the gun region, excluding the
            # head, belt, legs and April's ponytail. The endpoint is measured
            # from the PNG, independently of heroes.c's coordinates.
            points = [(x, y) for y in (range(30) if dy < 0 else range(35, 49))
                      for x in range(64) if torso.getpixel((x, y))[3]
                      and (dy < 0 or (x < 24 if dx < 0 else x > 41))]
            tip = max(points, key=lambda p: dx * p[0] + dy * p[1])
            assert (mx, my) == (tip[0] + tx, tip[1] + ty), (label, 'barrel', (mx, my), tip, (tx, ty))
        elif aim == 6:
            # The replacement down-aim gun extends past the waist. Its bottom
            # pixel is the barrel endpoint, rather than the inherited muzzle.
            points = [(x, y) for y in range(64) for x in range(64)
                      if torso.getpixel((x, y))[3]]
            tip = max(points, key=lambda p: p[1])
            assert (mx, my) == (tip[0] + tx, tip[1] + ty), (label, 'down barrel', (mx, my), tip, (tx, ty))
        if pose == 2:
            # At least five adjacent columns must join belt to pelvis at the
            # hip. Restrict to the pelvis so hair or stray sheet pixels cannot
            # pass the test on behalf of a detached body.
            joined = set()
            for y in range(35, 47):
                for x in range(24, 40):
                    if not legs.getpixel((x, y))[3]:
                        continue
                    for xx, yy in ((x, y), (x, y - 1)):
                        sx, sy = xx - tx, yy - ty
                        if 0 <= sx < 64 and 0 <= sy < 64 and torso.getpixel((sx, sy))[3]:
                            joined.add(x)
            assert any(set(range(x, x + 5)) <= joined for x in joined), (label, 'detached or pinched hip', joined)
        if tick == 0:
            image = legs.copy()
            if overlay >= 0:
                image.alpha_composite(torso, (ox, oy))
            if aim & 1 or aim == 6:
                draw = ImageDraw.Draw(image)
                dx = 0 if aim == 6 else -1 if aim in (1, 7) else 1
                dy = -1 if aim in (1, 3) else 1
                draw.line((mx, my, mx + dx * 12, my + dy * 12), fill=(0, 255, 255, 255))
            examples[h, side, pose, aim, shoot] = image
        checked += 1
    if args.sheet:
        items = sorted(examples.items())
        out = Image.new('RGB', (8 * 256, ((len(items) + 7) // 8) * 280), (40, 55, 75))
        draw = ImageDraw.Draw(out)
        for i, (key, image) in enumerate(items):
            x, y = i % 8 * 256, i // 8 * 280
            enlarged = image.resize((256, 256), Image.Resampling.NEAREST)
            out.paste(enlarged, (x, y + 24), enlarged)
            h, side, pose, aim, shoot = key
            draw.text((x + 4, y + 4), f'{("Saber", "April")[h]} {("stand", "run", "fall")[pose]} aim={aim} shoot={shoot}', fill='white')
        out.save(args.sheet)
    assert len(draw_frames) == checked
    print(f'{checked} poses passed: cropped run legs, connected falling hips, diagonal/down barrel origins, roof drops and first shots')


if __name__ == '__main__':
    main()
