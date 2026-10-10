#!/usr/bin/env python3
"""Check streamed SGX skies and native power restoration on the last stages."""
import argparse
import hashlib
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_sgx_motion import SKY_RECORD, check_camera_scroll
from test_sgx_rendering import Rendering, moon_cell

KEY_RIGHT, KEY_LEFT = 32, 128
POWER_STATE = 5
POWER_BAND_ROWS = range(7, 21)
POWER_BAND_COLS = 33


def sky_signature(r, e, stage):
    """Validate the visible sky and retain the VDC1 state across a power cut-in."""
    checked = r.sky(e)
    camera = r.metrics(e)['camera_x']
    scroll = r.word(e, 'pce_sgx_sky_scroll_x')
    blob = (r.out / f's{stage}.bin').read_bytes()
    record = struct.unpack_from(
        SKY_RECORD, blob, r.manifest['scenes'][stage - 1]['records']['sgx_sky_record']['offset'])
    expected = (camera * record[10] + 255) // 256
    registers = e.call('registers')['registers']
    bxr = registers['BXR']
    assert scroll == expected and bxr == (expected & 0x3FF), (
        stage, 'VDC1 sky scroll', camera, scroll, bxr, expected)
    near = r.word(e, 'pce_sgx_sky_near_x')
    split_line = e.memory(r.sym['pce_sgx_sky_split_line'], 1)[0]
    vram = bytes.fromhex(e.call('asread', 'vram1', 0, 65536)['hex'])
    page = e.memory(symbol(r.out / 'app.elf', 'pce_sgx_sky_page'), 1)[0] if record[17] else 0
    bat = page * 0x800
    column = r.word(e, 'pce_sgx_sky_first')
    visible = bytearray()
    for screen_col in range(33):
        world = column + screen_col
        for row in range(30):
            cell = struct.unpack_from('<H', vram, (bat + row * 64 + (world & 63)) * 2)[0]
            visible.extend(struct.pack('<H', cell))
            pattern = (cell & 0x0FFF) * 32
            visible.extend(vram[pattern:pattern + 32])
            # Keep the red-moon overlay tied to the exact camera phase too.
            moon = moon_cell(blob, record[17], camera, scroll, world, row)
            if moon is not None:
                assert vram[pattern:pattern + 32] == moon, (stage, world, row, 'moon pattern')
    return {
        'camera_x': camera,
        'scroll': scroll,
        'bxr': bxr,
        'near_scroll': near,
        'split_line': split_line,
        'visible_cells': checked,
        'visible_sha256': hashlib.sha256(visible).hexdigest(),
    }


def seed_platform(r, e, camera):
    """Seed a stable native walking state at one world camera position."""
    e.input(0)
    e.write(r.sym['actors'], bytes(168))
    e.write(r.sym['shots'], bytes(208))
    e.write(r.sym['player'], struct.pack('<4h4B', camera + 120, 160, 0, 0,
                                        0, 0, 4, 4))
    r.seed(e, 'camera', camera)
    r.seed(e, 'safe_timer', 250, 1)
    r.seed(e, 'dialogs_done', 255, 1)
    r.seed(e, 'cut_phase', 0, 1)
    r.seed(e, 'walk_in', 0, 1)
    e.run(90)
    r.settle(e)
    observed = r.metrics(e)['camera_x']
    assert observed == camera, ('seeded camera moved before input', camera, observed)


def native_wrap_walk(r, e, stage):
    """Cross BAT column 64 rightward, then walk back left on the native camera."""
    start = 488
    seed_platform(r, e, start)
    report = {'start_camera': start, 'right_samples': [], 'left_samples': []}

    e.input(KEY_RIGHT)
    previous = r.metrics(e)['camera_x']
    previous_player = r.metrics(e)['player_x']
    crossed_bat_column = False
    for _ in range(40):
        e.run(4)
        e.input(0)
        # Let the camera and the streamed sky publish the same presented
        # frame before checking their hardware scroll registers.
        for _ in range(8):
            r.settle(e)
            current = r.metrics(e)['camera_x']
            if r.word(e, 'pce_sgx_sky_scroll_x') == (current * 64 + 255) // 256:
                break
        else:
            raise AssertionError(('camera/sky did not settle at a presented frame',
                                  stage, current,
                                  r.word(e, 'pce_sgx_sky_scroll_x')))
        current = r.metrics(e)['camera_x']
        player = r.metrics(e)['player_x']
        assert current >= previous and player >= previous_player, (
            'native right walk reversed', previous, current, previous_player, player)
        check_camera_scroll(r, e, stage)
        if previous // 8 != current // 8:
            r.sky(e)
        if previous < 512 <= current:
            crossed_bat_column = True
            r.sky(e)
        report['right_samples'].append({'camera_x': current, 'player_x': player})
        previous, previous_player = current, player
        if current > 540:
            break
        e.input(KEY_RIGHT)
    e.input(0)
    assert previous > 512 and crossed_bat_column, (
        'native right walk did not cross the 512-pixel BAT wrap', report)
    right_camera, right_player = previous, previous_player

    # The platform camera intentionally scrolls only forward. Walk back toward
    # its left-edge wall and keep checking the already wrapped sky window.
    e.input(KEY_LEFT)
    previous_player = right_player
    for _ in range(40):
        e.run(4)
        e.input(0)
        for _ in range(8):
            r.settle(e)
            current = r.metrics(e)['camera_x']
            if r.word(e, 'pce_sgx_sky_scroll_x') == (current * 64 + 255) // 256:
                break
        else:
            raise AssertionError(('camera/sky did not settle on reverse walk', stage,
                                  current, r.word(e, 'pce_sgx_sky_scroll_x')))
        current = r.metrics(e)['camera_x']
        player = r.metrics(e)['player_x']
        assert current >= right_camera, ('side-scroller camera moved backward',
                                         right_camera, current)
        assert player <= previous_player, ('native left walk reversed',
                                           previous_player, player)
        check_camera_scroll(r, e, stage)
        report['left_samples'].append({'camera_x': current, 'player_x': player})
        previous_player = player
        if player < right_player - 48:
            break
        e.input(KEY_LEFT)
    e.input(0)
    assert previous_player < right_player - 48, (
        'native left walk did not return toward the left edge', report)
    r.sky(e)
    report['right_camera_after_wrap'] = right_camera
    report['left_walk_end_player_x'] = previous_player
    report['left_walk_kept_camera'] = True
    return report


def power_window_snapshot(r, e, stage):
    camera = r.metrics(e)['camera_x']
    vram = bytes.fromhex(e.call('asread', 'vram0', 0, 65536)['hex'])
    cells = []
    for row in POWER_BAND_ROWS:
        for x in range(POWER_BAND_COLS):
            col = ((camera >> 3) + x) & 63
            cells.append(struct.unpack_from('<H', vram, (row * 64 + col) * 2)[0])
    dead_panel_cells = [cell for cell in cells if 0x400 <= (cell & 0x0FFF) < 0x420]
    assert not dead_panel_cells, (
        'restored gameplay BAT references temporary power-panel tiles',
        stage, camera, dead_panel_cells[:8])
    palette15 = bytes.fromhex(e.call('asread', 'pram', 15 * 32, 32)['hex'])
    sky = sky_signature(r, e, stage)
    return {
        'camera_x': camera,
        'bat_cells': tuple(cells),
        # The power wave overlaps the first 1 KB of the font at word $4200;
        # the lower $4000-$41ff words are disposable dialogue-panel storage.
        'patterns': vram[0x8400:0x9000],
        'palette15': palette15,
        'sky': sky,
    }


def check_unified_stage4(r):
    scene = r.manifest['scenes'][3]
    blob = (r.out / 's4.bin').read_bytes()
    record = struct.unpack_from(
        SKY_RECORD, blob, scene['records']['sgx_sky_record']['offset'])
    assert record[10] == 64 and record[12] == 0 and record[16] == 0, (
        'stage 4 must use one quarter-speed panorama with no near map',
        record[10], record[12], record[16])
    return {'speed_q8': record[10], 'near_map': record[12],
            'split_row': record[16], 'columns': record[9]}


def exercise_power(r, e, stage, camera):
    seed_platform(r, e, camera)
    before = power_window_snapshot(r, e, stage)
    assert before['sky']['camera_x'] == camera, (stage, camera, before['sky'])
    r.field(e, 'state', POWER_STATE)
    r.field(e, 'timer', 0)
    for _ in range(140):
        e.run(1)
        if r.state(e)['state'] != POWER_STATE:
            break
    assert r.state(e)['state'] == 0, ('power cut-in did not return to play', stage, camera,
                                     r.state(e))
    e.run(1)
    after = power_window_snapshot(r, e, stage)
    assert after['camera_x'] == camera, (stage, camera, after['camera_x'])
    assert after['bat_cells'] == before['bat_cells'], (
        'power restore changed covered VDC0 BAT cells', stage, camera)
    assert after['patterns'] == before['patterns'], (
        'power restore changed VDC0 font patterns', stage, camera)
    assert after['palette15'] == before['palette15'], (
        'power restore did not restore BG palette 15', stage, camera,
        before['palette15'].hex(), after['palette15'].hex())
    assert after['sky'] == before['sky'], (
        'power restore changed the SGX sky', stage, camera,
        before['sky'], after['sky'])
    return {'camera_x': camera, 'covered_cells': len(after['bat_cells']),
            'font_pattern_bytes': len(after['patterns']),
            'temporary_panel_tiles_not_referenced': True, 'palette15_restored': True,
            'sky_cells': after['sky']['visible_cells'], 'sky_bxr': after['sky']['bxr']}


def verify(out, only_stage4=False):
    out = out.resolve()
    r = Rendering(out)
    r.sym.update({name: symbol(out / 'app.elf', name) for name in
                  ('cut_phase', 'pce_sgx_sky_split_line', 'walk_in')})
    report = {'stage4_unified_sky': check_unified_stage4(r)}
    with tempfile.TemporaryDirectory(prefix='sgx-last2-', dir=out) as base, \
            Emulator(out / 'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        assert r.metrics(e)['stage'] == 1
        r.dialogs(e)
        e.run(120)
        report['power_restore'] = {}
        stages = (4,) if only_stage4 else (1, 3, 4, 5)
        for stage in stages:
            if stage != 1:
                r.stage(e, stage)
            if stage == 4:
                report['stage4_unified_sky'] = check_unified_stage4(r)
                report['native_wrap_walk'] = native_wrap_walk(r, e, stage)
            checks = []
            for camera in (128, 510, 518, 1022, 1030):
                checks.append(exercise_power(r, e, stage, camera))
            report['power_restore'][str(stage)] = checks
            print(f'Stage {stage}: power restored at cameras 128, 510, 518, 1022, 1030',
                  flush=True)
    (out / 'sgx-last2-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    parser.add_argument('--only-stage4', action='store_true',
                        help='run the stage 4 walk and power checks for fixture debugging')
    args = parser.parse_args()
    verify(args.out, args.only_stage4)
