#!/usr/bin/env python3
"""Reject exposed parallax art in house openings, then check native BG uploads."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

import build_assets
from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering


def source_openings(work):
    bake = build_assets.scenery_background
    try:
        build_assets.scenery_background = lambda image, *_: image
        raw = build_assets.platform_background(1, work, sgx=True)[3]
    finally:
        build_assets.scenery_background = bake
    actual = build_assets.platform_background(1, work, sgx=True)[3]
    level, files = build_assets.levl.load_dump(work / 'stage1.layers')
    cover = np.zeros((240, raw.width), np.uint8)
    # Independently render the source layers in front of MidBGHyperjpr.
    for layer in level.layers:
        if layer.name not in ('Playfield', 'Platforms', 'Cars'):
            continue
        bank = (build_assets.levl.png_bank(files[layer.cblock], layer.cblock)
                if files[layer.cblock] else build_assets.levl.load_bank(
                    work / 'srgb' / f'{layer.cblock:08X}.srgb'))
        cover = np.maximum(cover, build_assets.levl.render_layer(
            layer, bank, 0, raw.width, 240, None)[..., 3])
    raw_pixels, actual_pixels = np.asarray(raw), np.asarray(actual)
    protected = cover >= 128
    assert np.array_equal(raw_pixels[protected], actual_pixels[protected]), 'Prop overwrote facade'
    openings = []
    for trigger in json.loads((work / 'stage1.json').read_text())['triggers']:
        kind = trigger['type']
        if kind not in (17, 18):
            continue
        crhc = {17: '9393E59B', 18: '20C6FAEF'}[kind]
        data = (work / f'{crhc}.levl').read_bytes()
        art_id = struct.unpack_from('<I', data, 4)[0]
        first = struct.unpack_from('<I', data, 0x50)[0]
        flags = struct.unpack_from('<I', data, 0x60)[0]
        art = work / 'srgb' / f'{art_id:08X}.srgb'
        cell = (build_assets.cblock_whole_frame(art, first) if flags & 8
                else build_assets.cblock_frame(art, first))
        ax, ay = map(round, struct.unpack_from('<ff', data, 8))
        x, y = trigger['waypoints'][0]
        region = (x - ax, y - ay, x - ax + cell.width, y - ay + cell.height)
        x0, y0, x1, y1 = region
        pixels = np.asarray(cell)
        exposed = (cover[y0:y1, x0:x1] < 128) & (pixels[..., 3] >= 128)
        assert exposed.sum() > 100, ('Fixture must contain an actual opening', kind)
        assert not np.array_equal(np.asarray(raw.crop(region))[exposed], pixels[exposed]), (
            'Fixture must distinguish missing prop from correct window', kind)
        assert np.array_equal(np.asarray(actual.crop(region))[exposed], pixels[exposed]), (
            'Window exposes unrelated background or a transparent hole', kind, region)
        openings.append(dict(type=kind, region=region, filled_pixels=int(exposed.sum())))
    assert len(openings) == 2
    return actual, openings


def visible_background(e, scene, blob, scroll_address):
    scroll = int.from_bytes(e.memory(scroll_address, 2), 'little')
    vram = bytes.fromhex(e.call('asread', 'vram0', 0, 65536)['hex'])
    records = scene['records']
    checked = 0
    # Interior columns are on screen under either side of the sub-tile scroll;
    # edge columns can be prepared for the next pending scroll publication.
    for world in range(scroll // 8 + 1, scroll // 8 + 32):
        for row in range(2, 28):
            tile, palette = struct.unpack_from('<HB', blob,
                records['sgx_bg_columns']['offset'] + world * 90 + row * 3)
            word = struct.unpack_from('<H', vram, (row * 64 + (world & 63)) * 2)[0]
            source = records['sgx_bg_patterns']['offset'] + tile * 32
            pattern = (word & 4095) * 32
            assert word >> 12 == palette and vram[pattern:pattern + 32] == blob[source:source + 32], (
                'Native house BAT/pattern mismatch', scroll, world, row, tile, hex(word))
            checked += 1
    return dict(scroll=scroll, cells=checked)


def verify(out):
    out = out.resolve()
    source, openings = source_openings(out / 'work')
    c = Rendering(out)
    scene = c.manifest['scenes'][0]
    for kind in (17, 18):
        assert scene['actor_ids'][kind] == 255, ('Full window actor overlay returned', kind)
        assert not any(s['name'] == f'actor_type{kind}' for s in scene['sprites'])
    blob = (out / 's1.bin').read_bytes()
    # Quantization/tile-budget reduction must also preserve opaque windows.
    # These authored 48x32 cells are entirely opaque in the source artwork.
    records = scene['records']
    for opening in openings:
        x0, y0, x1, y1 = opening['region']
        assert opening['filled_pixels'] == (x1 - x0) * (y1 - y0)
        for x in range(x0 // 8, x1 // 8):
            for y in range(y0 // 8, y1 // 8):
                tile = struct.unpack_from('<H', blob,
                    records['sgx_bg_columns']['offset'] + x * 90 + y * 3)[0]
                start = records['sgx_bg_patterns']['offset'] + tile * 32
                pattern = blob[start:start + 32]
                assert all(pattern[row * 2] | pattern[row * 2 + 1] |
                           pattern[16 + row * 2] | pattern[17 + row * 2] == 255
                           for row in range(8)), ('Baked window contains transparent pixels', x, y)
    scroll_address = symbol(out / 'app.elf', 'pce_scroll_x')
    checks = []
    with tempfile.TemporaryDirectory(prefix='house-openings-', dir=out) as base, Emulator(
            out / 'saber_rider.cue', base, sgx=True) as e:
        boot(e, c.address)
        c.dialogs(e)
        for name, camera in (('saloon', 6480), ('hotel', 7100)):
            source.crop((camera, 0, camera + 256, 224)).save(out / f'{name}-source-filled.png')
            # Cross tile and 256-pixel sector boundaries in both directions.
            for offset in (-16, -1, 0, 1, 7, 8, 16, 128, 16, 8, 0, -16):
                c.position(e, camera + offset + 120)
                c.settle(e)
                checks.append(visible_background(e, scene, blob, scroll_address))
            c.position(e, camera + 120)
            c.settle(e)
            e.screenshot(out / f'{name}-disc-filled.png')
        # Native motion through the reported hotel, with active shots/uploads.
        c.position(e, 7180)
        e.input(32 | 1)
        for _ in range(90):
            c.settle(e)
            checks.append(visible_background(e, scene, blob, scroll_address))
        e.input(0)
    assert len({c['scroll'] for c in checks[24:]}) > 30, 'Motion fixture did not scroll through hotel'
    report = dict(openings=openings, native_samples=len(checks),
                  native_cells=sum(c['cells'] for c in checks), checks=checks)
    (out / 'house-openings-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('House openings passed:', {k: v for k, v in report.items() if k != 'checks'}, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    verify(parser.parse_args().out)
