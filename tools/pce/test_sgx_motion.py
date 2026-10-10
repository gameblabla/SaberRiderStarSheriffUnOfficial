#!/usr/bin/env python3
"""Check SGX skies against the camera and verify their streamed BAT cells."""
import argparse
import json
import struct
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering,moon_cell

SKY_RECORD = '<IIHBIHIHHHHBIHHHBII'


def check_visible_sky(r, e, stage):
    """Compare all 33 visible columns, including both halves of split skies."""
    scene = r.manifest['scenes'][stage - 1]
    blob = (r.out / f's{stage}.bin').read_bytes()
    record = struct.unpack_from(SKY_RECORD, blob, scene['records']['sgx_sky_record']['offset'])
    (patterns, mapping, _tile_count, _cache, _fg, _nfg, _slow, _nslow,
     _first_tile, cols, _speed, wrap, near_map, near_cols, near_first,
     _near_speed, split_row, moon_patterns, _moon_palette) = record
    first = r.word(e, 'pce_sgx_sky_first')
    live_near_first = int.from_bytes(
        e.memory(symbol(r.out / 'app.elf', 'pce_sgx_sky_near_x'), 2), 'little') >> 3
    vram = bytes.fromhex(e.call('asread', 'vram1', 0, 65536)['hex'])
    camera=r.metrics(e)['camera_x'];scroll=r.word(e,'pce_sgx_sky_scroll_x')
    bat = (e.memory(symbol(r.out/'app.elf','pce_sgx_sky_page'),1)[0]*0x800 if moon_patterns else 0)
    checked = 0
    for screen_col in range(33):
        for y in range(30):
            near = bool(split_row and y >= split_row)
            source_map, source_cols, id_base = (
                (near_map, near_cols, near_first) if near else (mapping, cols, 0))
            world = (live_near_first if near else first) + screen_col
            source = world % source_cols if (wrap or near) else world
            if source < source_cols:
                tile, palette = struct.unpack_from('<HB', blob, source_map + source * 90 + y * 3)
                tile += id_base
            else:
                tile, palette = 0, 0
            cell = struct.unpack_from('<H', vram, (bat + y * 64 + (world & 63)) * 2)[0]
            pattern = (cell & 4095) * 16
            moon=moon_cell(blob,moon_patterns,camera,scroll,world,y)
            if moon is not None:
                assert cell>>12==14 and vram[pattern*2:pattern*2+32]==moon,(stage,world,y,'moon BG1 tile')
                checked+=1;continue
            assert cell >> 12 == palette, (
                stage, world, y, 'BAT palette', cell >> 12, palette)
            assert vram[pattern * 2:pattern * 2 + 32] == blob[
                patterns + tile * 32:patterns + (tile + 1) * 32
            ], (stage, world, y, 'BAT pattern', tile, id_base)
            checked += 1
    hidden = symbol(r.out / 'app.elf', 'pce_sgx_vdc1_hidden')
    assert e.memory(hidden, 1) == b'\0', (stage, 'VDC1 sky hidden')
    return checked


def check_camera_scroll(r, e, stage):
    scene = r.manifest['scenes'][stage - 1]
    blob = (r.out / f's{stage}.bin').read_bytes()
    record = struct.unpack_from(SKY_RECORD, blob, scene['records']['sgx_sky_record']['offset'])
    camera = r.metrics(e)['camera_x']
    main = r.word(e, 'pce_sgx_sky_scroll_x')
    near = int.from_bytes(e.memory(r.sym['pce_sgx_sky_near_x'], 2), 'little')
    expected_main = (camera * record[10] + 255) // 256
    expected_near = (camera * record[15] + 255) // 256
    first = r.word(e, 'pce_sgx_sky_first')
    assert main == expected_main, (stage, 'main sky camera offset', camera, main, expected_main)
    assert first == main >> 3, (stage, 'main BAT column', first, main)
    assert near == expected_near, (stage, 'near sky camera offset', camera, near, expected_near)
    # The debugger exposes the second VDC's BXR under the duplicate BXR key.
    # Hardware keeps only the low ten scroll bits, including at BAT wraps.
    bxr = e.call('registers')['registers']['BXR']
    assert bxr == (expected_main & 0x3FF), (
        stage, 'VDC1 BXR', camera, bxr, expected_main & 0x3FF)
    near_first = near >> 3
    return {'camera_x': camera, 'main_scroll': main, 'expected_main': expected_main,
            'near_scroll': near, 'expected_near': expected_near, 'bat_first': first,
            'main_window_crosses_bat': (first & 63) + 32 >= 64,
            'near_window_crosses_bat': (near_first & 63) + 32 >= 64}


def seed_platform(e, r, camera, player_x):
    """Set a grounded platform state at pixel coordinates for controlled motion."""
    e.input(0)
    e.write(r.sym['actors'], bytes(168))
    e.write(r.sym['shots'], bytes(208))
    e.write(r.sym['player'], struct.pack('<4h4B', player_x, 160, 0, 0, 0, 0, 4, 4))
    e.write(r.sym['camera'], struct.pack('<H', camera))
    e.write(r.sym['safe_timer'], b'\0')
    e.write(r.sym['cut_phase'], b'\0')
    e.run(4)


def run(out):
    out = out.resolve()
    r = Rendering(out)
    r.sym.update({name: symbol(out / 'app.elf', name) for name in
                  ('camera', 'cut_phase', 'pce_scroll_x', 'pce_sgx_sky_near_x',
                   'pce_sgx_sky_split_line', 'pce_sgx_vdc1_hidden', 'walk_in')})
    report = {'camera_samples': [], 'visible_sky_cells': {}}
    with Emulator(out / 'saber_rider.cue', out / 'motion-emulator', sgx=True) as e:
        boot(e, r.address)
        assert r.metrics(e)['stage'] == 1
        # Prove the initial retail fade exposes a populated SGX sky.
        e.screenshot(out / 'motion-startup-stage1.png')
        report['visible_sky_cells']['startup_stage1'] = check_visible_sky(r, e, 1)
        r.dialogs(e)

        # Let the native camera catch up with neutral input. The player's
        # world position stays fixed while camera_x advances toward player_x-120.
        camera0 = 4000
        seed_platform(e, r, camera0, camera0 + 220)
        player_before = int.from_bytes(e.memory(r.sym['player'], 2), 'little', signed=True)
        sky_before = r.word(e, 'pce_sgx_sky_scroll_x')
        e.input(0)
        e.run(90)
        r.settle(e)
        pan = r.metrics(e)
        scroll = r.word(e, 'pce_sgx_sky_scroll_x')
        scene = r.manifest['scenes'][0]
        blob = (out / 's1.bin').read_bytes()
        rec = struct.unpack_from(SKY_RECORD, blob, scene['records']['sgx_sky_record']['offset'])
        speed = rec[10]
        expected = (pan['camera_x'] * speed + 255) // 256
        assert pan['player_x'] == player_before == camera0 + 220, (
            'camera-pan fixture moved player', player_before, pan)
        assert pan['camera_x'] > camera0, ('native camera did not catch up', camera0, pan)
        assert scroll != sky_before, ('stationary-player camera motion left sky unchanged',
                                      sky_before, scroll, pan)
        assert scroll == expected, ('sky did not follow a stationary player camera pan',
                                    pan['camera_x'], scroll, expected, speed)
        report['camera_samples'].append({
            'case': 'stationary_player_camera_pan', 'player_x': pan['player_x'],
            **check_camera_scroll(r, e, 1)})
        report['visible_sky_cells']['camera_pan_stage1'] = check_visible_sky(r, e, 1)

        # With the camera fixed, moving the player away from its follow edge
        # must leave both the camera and the sky unchanged.
        camera1 = pan['camera_x']
        seed_platform(e, r, camera1, camera1 + 100)
        sky_before = r.word(e, 'pce_sgx_sky_scroll_x')
        player_before = int.from_bytes(e.memory(r.sym['player'], 2), 'little', signed=True)
        r.press(e, 0x80, 15)
        r.settle(e)
        moved = r.metrics(e)
        sky_after = r.word(e, 'pce_sgx_sky_scroll_x')
        assert moved['player_x'] < player_before, ('left input did not move player', player_before, moved)
        assert moved['camera_x'] == camera1, ('camera fixture followed player unexpectedly', camera1, moved)
        assert sky_after == sky_before, ('player motion moved sky with a fixed camera', sky_before, sky_after)
        report['camera_samples'].append({
            'case': 'moving_player_fixed_camera', 'player_before': player_before,
            'player_after': moved['player_x'], 'camera_x': moved['camera_x'],
            'sky_before': sky_before, 'sky_after': sky_after,
            **check_camera_scroll(r, e, 1)})

        # Independent source-content boundary: level 1 retains 512 source
        # pixels, so camera 2048 repeats it. Keep partial tile offsets and
        # revisit in reverse, then check a second repetition.
        for camera_seed in (2020, 2044, 2048, 2052, 2076, 2052, 2048, 2044,
                            4092, 4096, 4100):
            seed_platform(e,r,camera_seed,camera_seed+100)
            r.seed(e,'dialogs_done',255,1)
            e.run(40);r.settle(e)
            sample=check_camera_scroll(r,e,1)
            assert sample['camera_x']==camera_seed,sample
            assert sample['main_scroll']==(camera_seed+3)//4,sample
            checks=check_visible_sky(r,e,1)
            report.setdefault('repeat_boundary_samples',[]).append(dict(**sample,cells=checks))
            e.screenshot(out/f'motion-repeat-{len(report["repeat_boundary_samples"]):02d}-{camera_seed}.png')

        # Verify source wrapping and BAT wrapping at late, distinct native
        # positions for all platform scenes with an SGX sky, including stage 4's split band.
        # Keep each camera below any stage-ending trigger while deliberately
        # placing the 33-column viewport across a 64-column BAT edge.
        windows = {1: (4000, 4100), 3: (6000, 6100), 4: (5000, 5100),
                   5: (6464, 6564)}
        for stage in (1, 3, 4, 5):
            if stage != 1:
                r.stage(e, stage)
                e.write(r.sym['walk_in'], b'\0')
            camera_seed, player_seed = windows[stage]
            seed_platform(e, r, camera_seed, player_seed)
            r.seed(e, 'dialogs_done', 255, 1)
            e.run(90)
            r.settle(e)
            checks = check_visible_sky(r, e, stage)
            camera_state = check_camera_scroll(r, e, stage)
            assert camera_state['main_window_crosses_bat'], (stage, 'main BAT edge not crossed', camera_state)
            report['visible_sky_cells'][f'stage{stage}_sample'] = checks
            if stage == 4:
                record = struct.unpack_from(
                    SKY_RECORD, (out / 's4.bin').read_bytes(),
                    r.manifest['scenes'][3]['records']['sgx_sky_record']['offset'])
                assert record[10] == 256 and record[12] == 0 and record[16] == 0, (
                    'stage 4 sky must use one full-speed panorama',
                    record[10], record[12], record[16])
                assert r.word(e, 'pce_sgx_sky_near_x') == 0
                assert e.memory(r.sym['pce_sgx_sky_split_line'], 1) == b'\0'
                assert camera_state['main_scroll'] == camera_state['camera_x'], camera_state
            report.setdefault('camera_by_stage', {})[str(stage)] = camera_state
            r.metrics(e)

    (out / 'motion-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    run(parser.parse_args().out)
