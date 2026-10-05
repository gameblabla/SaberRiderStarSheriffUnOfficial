#!/usr/bin/env python3
"""Dialogue must not recolor shared-palette sprites.

Stage 4 keeps a foreground layer (cabin walls) in the sprite cache's shared
slots, which all render with sprite palette 31. Opening a platform dialogue
used to upload the box corners' own palette to 31 (fixed patterns outside the
cache), so the foreground drew with corner colours for the whole dialogue -
seen as the tower sniper "glitching out" during the pre-boss ambush scene.

The corners are ordinary cached sprites now (their own palettes in private
slots), so palette 31 must stay the foreground's on every dialogue page and
after closing. Needs a debug (RETAIL=0) build for the stage menu.
"""
import argparse
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_campaign import Campaign


def fg_palette(out, stage):
    manifest = json.loads((out/'manifest.json').read_text())
    scene = [s for s in manifest['scenes'] if s['stage'] == stage][0]
    names = [s['name'] for s in scene['sprites']]
    blob = (out/f's{stage}.bin').read_bytes()
    table = blob[scene['records']['sprite_table']['offset']:]
    first = next(i for i, n in enumerate(names) if n.startswith('foreground_'))
    off, desc, pal, count, w, h = struct.unpack_from('<IIIHBB', table, first*16)
    return blob[pal:pal+32]


def verify(out):
    c = Campaign(out)
    elf = out/'app.elf'
    review = out/'dialog-palette-review'
    review.mkdir(exist_ok=True)
    want = fg_palette(out, 4)
    report = {}
    with tempfile.TemporaryDirectory(prefix='dialog-palette-', dir=out) as base:
        with Emulator(out/'saber_rider.cue', base) as e:
            boot(e, c.address)
            e.run(120)
            c.stage(e, 4)
            c.dialogs(e)
            c.move(e, 5600, 160)
            c.seed(e, 'dialogs_done', 255, 1)
            e.run(30)
            # Walk into the arena like a player: the placed tower sniper and
            # the ambush dialogue trigger for real.
            e.input(32)
            opened = False
            for _ in range(300):
                c.seed(e, 'safe_timer', 250, 1)
                e.run(2)
                if c.state(e)['state'] == 1:
                    opened = True
                    break
            e.input(0)
            assert opened, 'ambush dialogue never opened'
            e.run(30)  # let the opened panel present before capturing
            pool = e.memory(symbol(elf, 'actors'), 8*21)
            kinds = [(pool[i*21+13], struct.unpack_from('<2h', pool, i*21)) for i in range(8) if pool[i*21+12]]
            assert any(t == 31 for t, _ in kinds), f'tower sniper missing: {kinds}'
            report['tower_sniper'] = True
            for page in range(4):
                if page:
                    c.press(e, 1, 15)
                    e.run(100)
                st = c.state(e)
                if st['state'] != 1:
                    break
                got = bytes.fromhex(e.call('asread', 'pram', 31*32, 32)['hex'])
                assert got == want, f'page {page}: shared palette 31 is not the foreground one'
                m = c.metrics(e)
                e.screenshot(review/f'ambush-page{page}.png')
                report[f'page{page}'] = dict(sat_count=m['sat_count'], max_units=m['max_units'])
            c.dialogs(e)
            e.run(30)
            got = bytes.fromhex(e.call('asread', 'pram', 31*32, 32)['hex'])
            assert got == want, 'shared palette 31 not restored after the dialogue'
            m = c.metrics(e)
            e.screenshot(review/'ambush-closed.png')
            report['closed'] = dict(sat_count=m['sat_count'])
    report['shared_palette'] = 'foreground palette on every page and after closing'
    (out/'dialog-palette-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(report)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/pce'))
    verify(p.parse_args().out.resolve())
