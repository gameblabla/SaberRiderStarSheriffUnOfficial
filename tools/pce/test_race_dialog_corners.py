#!/usr/bin/env python3
"""Check race dialogue alpha corners, every opening page, and sky restoration."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from emulator import Emulator, boot, symbol
from test_campaign import Campaign
from test_port import decode_sprite
from test_sgx_reported_issues import read_space


def verify(out, sgx):
    out = out.resolve()
    c = Campaign(out, sgx=sgx)
    scene = json.loads((out/'manifest.json').read_text())['scenes'][1]
    blob = (out/'s2.bin').read_bytes()
    elf = out/'app.elf'
    captures = out/'race-dialog-review'
    captures.mkdir(exist_ok=True)
    report = []
    with tempfile.TemporaryDirectory(dir=out) as base, Emulator(out/'saber_rider.cue', base, sgx=sgx) as e:
        boot(e, c.address)
        c.field(e, 'state', 2)
        c.advance(e, 2)
        assert c.state(e)['state'] == 1, 'Opening race dialogue missing'
        kept = read_space(e, 'acram', 0x1e0000, 672)
        first_image = None
        first_mask = None
        for _ in range(20):
            if c.state(e)['state'] != 1:
                break
            e.run(120)
            page = c.state(e)['page']
            for row in (0, 1, 4, 5):
                for col in (0, 52):
                    actual = read_space(e, 'vram0', ((51+row)*128+6+col)*2, 8)
                    assert actual == kept[row*112+col*2:row*112+col*2+8], (
                        'Race corner replaced its sky with panel background', page, row, col)
            slots = e.memory(symbol(elf, 'sprite_slot_of'), scene['sprite_count'])
            words = struct.unpack('<48H', e.memory(symbol(elf, 'sprite_words'), 96))
            sat = list(struct.iter_unpack('<4H', read_space(e, 'sat0', 0, 512)))
            for sprite_id, sprite in enumerate(scene['sprites']):
                if not sprite['name'].startswith('portrait_'):
                    continue
                slot = slots[sprite_id]
                if slot >= 48:
                    continue
                _pat, _desc, pal, count, _w, _h = struct.unpack_from(
                    '<IIIHBB', blob, scene['sprite_table']+sprite_id*16)
                hits = [s for s in sat if s[0] and words[slot]>>5 <= s[2] < (words[slot]+count*64)>>5]
                if hits:
                    assert read_space(e, 'pram', (16+(hits[0][3]&15))*32, 32) == blob[pal:pal+32], (
                        'Corner sprites recoloured the portrait', page, sprite['name'])
            box = scene['presentation']['dialog']
            live = []
            for colour in range(4):
                mask = np.zeros((48, 448), dtype=bool)
                entries = []
                for half in range(2):
                    sprite = box+colour*2+half
                    slot = slots[sprite]
                    if slot >= 48:
                        continue
                    pat, desc, _pal, count, _w, _h = struct.unpack_from(
                        '<IIIHBB', blob, scene['sprite_table']+sprite*16)
                    hits = [s for s in sat if s[0] and words[slot]>>5 <= s[2] < (words[slot]+count*64)>>5]
                    if not hits:
                        continue
                    entries.extend(hits)
                    palette = read_space(e, 'pram', (16+(hits[0][3]&15))*32, 32)
                    assert palette == blob[_pal:_pal+32], ('Corner palette changed', page, sprite)
                    for y, x, pattern, attr in hits:
                        assert attr & 0x80, ('Corner hidden behind the sky', page, sprite)
                        offset = (pattern-(words[slot]>>5))*64
                        size = 256 if attr & 0x100 else 128
                        assert read_space(e, 'vram0', pattern*64, size) == blob[pat+offset:pat+offset+size]
                    for x, y, index in struct.iter_unpack('<hhH', blob[desc:desc+count*6]):
                        if index & 0x4000:
                            continue
                        wide = bool(index & 0x8000)
                        index &= 0x3fff
                        for piece in range(2 if wide else 1):
                            pixels = decode_sprite(blob[pat+(index+piece)*128:pat+(index+piece+1)*128])
                            sx = half*224+x+piece*16
                            mask[y:y+16, sx:sx+16] = pixels != 0
                if entries:
                    live.append((colour, entries, mask))
            assert len(live) == 1, ('Missing or stale race corner colour', page, live)
            colour, entries, mask = live[0]
            assert {(y-64, x-32) for y, x, p, a in entries} >= {
                (24, 48), (24, 464), (56, 48), (56, 464)}, entries
            path = captures/f'page-{page}.png'
            e.screenshot(path)
            if first_image is None:
                first_image = np.asarray(Image.open(path))
                corner_blocks = np.zeros_like(mask)
                for x in (0, 416):
                    for y in (0, 32):
                        corner_blocks[y:y+16, x:x+32] = True
                first_mask = corner_blocks & ~mask
                # The speaker portrait overlaps the leftmost 16 dots of the
                # top-left corner, so those pixels belong to the portrait.
                first_mask[:24, :16] = False
                assert first_mask.sum() > 0, 'Corner art has no transparent pixels'
            report.append(dict(page=page, colour=colour, entries=len(entries)))
            c.press(e, 1, 15)
        assert c.state(e)['state'] != 1, 'Opening dialogue did not close'
        e.run(60)
        actual = b''.join(read_space(e, 'vram0', ((51+row)*128+6)*2, 112) for row in range(6))
        assert actual == kept, 'Race sky did not restore after dialogue'
        path = captures/'closed.png'
        e.screenshot(path)
        closed = np.asarray(Image.open(path))
        # The emulator captures the 512-dot playfield with 24 dots of border
        # at the left and 11 scanlines above it. The race sky remains frozen.
        before = first_image[35:83, 72:520]
        after = closed[35:83, 72:520]
        # Resuming the race can shift the sky's alternating dither by a dot.
        # Compare the solid pixels; BAT checks above cover every corner cell.
        solid = (after == np.roll(after, 1, axis=1)).all(axis=2)
        solid &= (after == np.roll(after, -1, axis=1)).all(axis=2)
        solid &= (after == np.roll(after, 2, axis=1)).all(axis=2)
        solid &= (after == np.roll(after, -2, axis=1)).all(axis=2)
        visible = first_mask & solid
        assert visible.sum() >= 20, 'No solid sky pixels exercised through corners'
        assert np.array_equal(before[visible], after[visible]), 'Transparent corners do not show the sky'
    (out/'race-dialog-corners-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Race dialogue corners passed', report, flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/pce'))
    p.add_argument('--sgx', action='store_true')
    args = p.parse_args()
    verify(args.out, args.sgx)
