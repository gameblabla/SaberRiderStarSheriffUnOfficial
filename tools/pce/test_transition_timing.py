#!/usr/bin/env python3
"""Check the stage-three camera is published before its first visible fade."""
import argparse
import json
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_campaign import Campaign


def verify(out: Path, sgx: bool) -> dict:
    out = out.resolve()
    game = Campaign(out, sgx=sgx)
    names = [
        'stage', 'pce_display_on', 'camera', 'pce_scroll_x',
        'pce_sat_scroll_x', 'fg_camera', 'fg_camera_slow',
    ]
    if sgx:
        names.append('pce_sgx_sky_scroll_x')
    symbols = {name: symbol(out / 'app.elf', name) for name in names}
    first_visible = None

    with tempfile.TemporaryDirectory(prefix='transition-timing-', dir=out) as base, \
            Emulator(out / 'saber_rider.cue', base, sgx=sgx) as e:
        boot(e, game.address)
        assert game.metrics(e)['stage'] == 1, game.metrics(e)

        # Seed the retail clear path so the real loader and change_stage code
        # enter stage 3, including its regular walk-in initialization/fade.
        e.write(symbols['stage'], b'\x02')
        game.field(e, 'state', 2)

        ready = False
        saw_hidden_black = False
        for emu_frame in range(12000):
            e.run(1)
            m = game.metrics(e)
            if m['stage'] != 3:
                continue

            # Palette RAM is the presentation source: its first non-black
            # entry marks the first frame on which the fade can be seen.
            palette = bytes.fromhex(e.call('asread', 'pram', 0, 1024)['hex'])
            display_on = e.memory(symbols['pce_display_on'], 1)[0]
            nonblack = any(palette)
            if not display_on and not nonblack:
                saw_hidden_black = True
            if saw_hidden_black and display_on and nonblack:
                regs = e.call('registers')['registers']
                record = {
                    'emulator_frame': emu_frame,
                    'telemetry_frame': m['frames'],
                    'ready': m['ready'],
                    'display_on': display_on,
                    'camera': int.from_bytes(e.memory(symbols['camera'], 2), 'little'),
                    'camera_x': m['camera_x'],
                    'pce_scroll_x': int.from_bytes(e.memory(symbols['pce_scroll_x'], 2), 'little'),
                    'sat_scroll_x': int.from_bytes(e.memory(symbols['pce_sat_scroll_x'], 2), 'little'),
                    'fg_camera': int.from_bytes(e.memory(symbols['fg_camera'], 2), 'little'),
                    'fg_camera_slow': int.from_bytes(e.memory(symbols['fg_camera_slow'], 2), 'little'),
                    'vdc_bxr': regs['BXR'],
                }
                if sgx:
                    record['sky_scroll_x'] = int.from_bytes(
                        e.memory(symbols['pce_sgx_sky_scroll_x'], 2), 'little')
                first_visible = record
                break
            if m['ready']:
                ready = True
                break

        assert first_visible is not None, ('Stage 3 never showed a non-black fade frame after its hidden black state', ready)
        assert first_visible['ready'] == 0, ('Fade began after stage became ready', first_visible)
        assert first_visible['camera'] == 48, ('Stage 3 walk-in camera', first_visible)
        assert first_visible['camera_x'] == 48, ('Telemetry camera was stale at fade start', first_visible)
        assert first_visible['fg_camera'] == 48, ('Foreground camera was stale at fade start', first_visible)
        assert first_visible['pce_scroll_x'] == 48, ('Background camera was stale at fade start', first_visible)
        assert first_visible['sat_scroll_x'] == 48, ('Sprite camera was stale at fade start', first_visible)
        if sgx:
            assert first_visible['vdc_bxr'] == first_visible['sky_scroll_x'], (
                'VDC-A BXR differs from the streamed sky scroll', first_visible)
        else:
            assert first_visible['vdc_bxr'] == 48, ('VDC BXR was stale at fade start', first_visible)

    return first_visible


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/pce'))
    parser.add_argument('--sgx', action='store_true')
    args = parser.parse_args()
    result = verify(args.out, args.sgx)
    (args.out / 'transition-timing-verification.json').write_text(
        json.dumps(result, indent=2) + '\n')
    print('Stage 3 transition timing passed', result, flush=True)
