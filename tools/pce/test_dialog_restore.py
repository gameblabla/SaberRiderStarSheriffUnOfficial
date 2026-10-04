#!/usr/bin/env python3
"""Dialogue BAT/font restoration and sprite budgets with real native paging."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from emulator import Emulator, boot, symbol
from formats import indexed, pack_sprite
from test_campaign import Campaign
from test_port import decode_sprite


def packing():
    # A shifted 32-pixel strip formerly consumed three 16-pixel columns.
    im = Image.new('RGBA', (64, 32))
    im.paste((255, 255, 255, 255), (7, 0, 39, 16))
    im.paste((255, 255, 255, 255), (23, 16, 55, 32))
    data, pieces, palette, peak = pack_sprite(im, (32, 16))
    assert len(pieces) == 4 and peak == 2
    for flip in (False, True):
        actual = np.zeros((32, 96), np.uint8)
        for dx, dy, pattern in pieces:
            cell = decode_sprite(data[pattern*128:(pattern+1)*128])
            if flip:
                dx = -dx-16
                cell = cell[:, ::-1]
            actual[dy+16:dy+32, dx+48:dx+64] = cell
        expected = indexed(im, palette)
        if flip:
            expected = expected[:, ::-1]
        assert np.array_equal(actual[:, 16:80], expected)


def verify(out):
    packing()
    c = Campaign(out)
    elf = out/'app.elf'
    manifest = json.loads((out/'manifest.json').read_text())
    for scene in manifest['scenes']:
        if scene['stage'] not in (1, 3, 4, 5):
            continue
        boxes = [s for s in scene['sprites'] if s['name'].startswith('dialog_box')]
        assert len(boxes) == 8 and all(s['entries'] == 2 for s in boxes)
        blob = (out/f"s{scene['stage']}.bin").read_bytes()
        mapping = scene['records']['bg_columns']
        assert 15 not in blob[mapping['offset']+2:mapping['offset']+mapping['bytes']:3]
    captures = out/'dialog-review'
    captures.mkdir(exist_ok=True)
    checks = []
    with tempfile.TemporaryDirectory(prefix='dialog-restore-', dir=out) as base, Emulator(out/'saber_rider.cue', base) as e:
        boot(e, c.address)
        scene = manifest['scenes'][0]
        blob = (out/'s1.bin').read_bytes()
        original_font = (out/'font.bin').read_bytes()
        original_palette = blob[scene['pal']+15*32:scene['pal']+16*32-2]+b'\xff\x01'
        for phase in (0, 5, 7):
            c.press(e, 8)
            e.run(60)
            camera = 2120+phase
            c.seed(e, 'camera', camera)
            c.seed(e, 'dialogs_done', 3, 1)
            e.write(symbol(elf, 'actors'), bytes(8*21))
            e.write(symbol(elf, 'shots'), bytes(16*13))
            e.write(symbol(elf, 'player'), struct.pack('<4h4B', 2245, 177, 0, 0, 0, 0, 4, 4))
            c.press(e, 8)
            c.until(e, lambda: c.state(e)['state'] == 1, limit=600, step=1)
            e.run(100)
            e.screenshot(captures/f'panel-{phase}.png')
            if phase == 5:
                c.press(e, 8)
                e.run(30)
                assert bytes.fromhex(e.call('asread', 'vram0', 0x8400, 3072)['hex']) == original_font
                assert bytes.fromhex(e.call('asread', 'pram', 480, 32)['hex']) == original_palette
                c.seed(e,'pce_scroll_y',3)
                c.press(e, 8)
                e.run(100)
                assert c.state(e)['state'] == 1
                assert e.memory(symbol(elf,'pce_scroll_y'),2)==b'\0\0','Dialogue must undo world rumble'
            # The next page is the stampede warning in the supplied save.
            c.press(e, 1, 15)
            e.run(120)
            e.screenshot(captures/f'stampede-warning-{phase}.png')
            scroll = int.from_bytes(e.memory(symbol(elf, 'pce_scroll_x'), 2), 'little')
            columns = struct.unpack('<990H', e.memory(symbol(elf, 'columns'), 1980))
            bat = struct.unpack('<2048H', bytes.fromhex(e.call('asread', 'vram0', 0, 4096)['hex']))
            for x in range(33):
                world = (scroll >> 3)+x
                for row in range(30):
                    if 3 <= x < 31 and 5 <= row < 11:
                        continue
                    palette = blob[scene['map']+(world % scene['cols'])*90+row*3+2]
                    expected = 128+columns[(world % 33)*30+row]+(palette << 12)
                    assert bat[row*64+(world & 63)] == expected, ('outside panel', phase, x, row)
            # Exact admissions must match the hardware SAT's width units.
            assert e.memory(symbol(elf, 'sprite_exact'), 1) == b'\1'
            sat = bytes.fromhex(e.call('asread', 'vram0', 0xfe00, 512)['hex'])
            lines = [0]*240
            for y, x, pattern, attr in struct.iter_unpack('<4H', sat):
                if not y:
                    continue
                y = (y & 1023)-64
                height = (16, 32, 64, 64)[(attr >> 12) & 3]
                for line in range(max(0, y), min(240, y+height)):
                    lines[line] += 2 if attr & 0x100 else 1
            assert max(lines) <= 16
            # Catch-up across a BAT column must restore the original panel
            # footprint, even when the next play draw scrolls further right.
            panel_scroll = int.from_bytes(e.memory(symbol(elf, 'pce_scroll_x'), 2), 'little')
            c.seed(e, 'camera', panel_scroll+4)
            c.dialogs(e)
            e.input(0)
            e.run(20)
            assert bytes.fromhex(e.call('asread', 'vram0', 0x8400, 3072)['hex']) == original_font
            assert bytes.fromhex(e.call('asread', 'pram', 480, 32)['hex']) == original_palette
            columns = struct.unpack('<990H', e.memory(symbol(elf, 'columns'), 1980))
            scroll = int.from_bytes(e.memory(symbol(elf, 'pce_scroll_x'), 2), 'little')
            bat = struct.unpack('<2048H', bytes.fromhex(e.call('asread', 'vram0', 0, 4096)['hex']))
            # Include the old left edge as well as the complete new viewport.
            for world in range(camera >> 3, (scroll >> 3)+33):
                if world < scroll >> 3:
                    continue  # this column was evicted, and will be loaded on return
                for row in range(30):
                    palette = blob[scene['map']+(world % scene['cols'])*90+row*3+2]
                    expected = 128+columns[(world % 33)*30+row]+(palette << 12)
                    assert bat[row*64+(world & 63)] == expected, (phase, world, row)
            c.metrics(e)
            e.screenshot(captures/f'restored-{phase}.png')
            checks.append(dict(seed_scroll_phase=phase, panel_scroll_phase=panel_scroll & 7,
                               panel_peak=max(lines), restored_cells=33*30))
    report = dict(packing='lossless, both facings', panels=checks, font='restored', palette='restored')
    (out/'dialog-restore-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(report)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/pce'))
    verify(p.parse_args().out.resolve())
