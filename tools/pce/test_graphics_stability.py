#!/usr/bin/env python3
"""Shared April colors, streamed scenery integrity and frozen race dialogue."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_campaign import Campaign


def verify(out):
    manifest = json.loads((out/'manifest.json').read_text())
    palettes = set()
    poses = 0
    for scene in manifest['scenes']:
        blob = (out/f"s{scene['stage']}.bin").read_bytes()
        records = scene['records']
        for sprite in scene['sprites']:
            if sprite['name'].startswith('hero2_'):
                record = records[sprite['name']+'_palette']
                palettes.add(blob[record['offset']:record['offset']+32])
                poses += 1
    assert poses and len(palettes) == 1, 'April must use one color mapping across every pose/stage'

    c = Campaign(out)
    elf = out/'app.elf'
    addresses = {n: symbol(elf, n) for n in
                 'columns cache_ids pce_sky_far pce_sky_near rphase phase_t finish_rank pce_race_dialog race_world_count'.split()}
    review = out/'graphics-stability-review'
    review.mkdir(exist_ok=True)

    def vram(e, start, size):
        return bytes.fromhex(e.call('asread', 'vram0', start, size)['hex'])

    with tempfile.TemporaryDirectory(prefix='graphics-stability-', dir=out) as base, Emulator(out/'saber_rider.cue', base) as e:
        boot(e, c.address)
        scene = manifest['scenes'][0]
        blob = (out/'s1.bin').read_bytes()
        # Camera props and the saloon: inspect every visible BAT entry and
        # its actual pattern, rather than just checking cache metadata.
        for camera in (5800, 6154, 7384):
            c.move(e, camera+120)
            c.seed(e, 'dialogs_done', 255, 1)
            e.run(60)
            scroll = c.metrics(e)['camera_x']
            columns = struct.unpack('<990H', e.memory(addresses['columns'], 1980))
            ids = struct.unpack('<896H', e.memory(addresses['cache_ids'], 1792))
            memory = vram(e, 0, 0x8000)
            for world in range(scroll >> 3, (scroll >> 3)+33):
                for row in range(30):
                    offset = scene['map']+(world % scene['cols'])*90+row*3
                    tile, palette = struct.unpack_from('<HB', blob, offset)
                    slot = columns[(world % 33)*30+row]
                    assert slot < 896 and ids[slot] == tile, (world, row, tile, slot)
                    bat = struct.unpack_from('<H', memory, (row*64+(world & 63))*2)[0]
                    assert bat == 128+slot+(palette << 12), (world, row, bat)
                    assert memory[0x1000+slot*32:0x1000+(slot+1)*32] == blob[scene['tiles']+tile*32:scene['tiles']+(tile+1)*32], (world, row, 'pattern')
            e.screenshot(review/f'stage1-{camera}.png')

        c.stage(e, 2)
        c.dialogs(e)
        e.input(16)
        e.run(300)
        e.input(0)
        # Seed a won finish with the existing curved-road frame still visible.
        # Native story_start, typing, paging and pursuit setup do the transition.
        c.seed(e, 'rphase', 2, 1)
        c.seed(e, 'phase_t', 192)
        c.seed(e, 'finish_rank', 1, 1)
        c.field(e, 'story', 1)
        c.field(e, 'event', 1)
        before = vram(e, 0, 0x8000)
        tables = e.memory(addresses['columns'], 768)
        sky = e.memory(addresses['pce_sky_far'], 2), e.memory(addresses['pce_sky_near'], 2)
        c.until(e, lambda: c.state(e)['state'] == 1, limit=600, step=1)
        e.run(120)
        assert e.memory(addresses['rphase'], 1) == b'\2', 'Pursuit must wait for the finish dialogue'
        assert e.memory(addresses['pce_race_dialog'], 1) == b'\1'
        assert e.memory(addresses['race_world_count'], 1)[0], 'Retain actual car SAT entries'

        def frozen():
            after = vram(e, 0, 0x8000)
            assert after[:48*256] == before[:48*256], 'Dialogue changed road BAT/wrap copies'
            assert after[0x4000:] == before[0x4000:], 'Dialogue changed road or sky patterns'
            for row in range(48, 64):
                start, end = row*256, (row+1)*256
                if 53 <= row < 59:
                    assert after[start:start+12] == before[start:start+12]
                    assert after[start+124:end] == before[start+124:end]
                else:
                    assert after[start:end] == before[start:end], ('sky BAT', row)
            assert e.memory(addresses['columns'], 768) == tables, 'Dialogue changed road raster tables'
            assert (e.memory(addresses['pce_sky_far'], 2), e.memory(addresses['pce_sky_near'], 2)) == sky

        frozen()
        e.screenshot(review/'race-finish-dialog.png')
        # Check subsequent pages while the native page handler is running.
        for _ in range(20):
            c.press(e, 1, 15)
            if c.state(e)['state'] != 1:
                break
            frozen()
        else:
            raise AssertionError('Finish dialogue did not close')
        c.until(e, lambda: e.memory(addresses['rphase'], 1) == b'\3', limit=600, step=1)
        assert e.memory(addresses['pce_race_dialog'], 1) == b'\0'
        e.screenshot(review/'race-pursuit.png')

    report = dict(april_poses=poses, april_palettes=len(palettes),
                  background_windows=3, race_dialogue='road/sky assets and raster tables preserved; pursuit deferred')
    (out/'graphics-stability-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/pce'))
    verify(parser.parse_args().out.resolve())
