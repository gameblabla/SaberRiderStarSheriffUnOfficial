#!/usr/bin/env python3
"""Keep cached HUD colours when a real power tap loads a new counter digit."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot
from test_sgx_rendering import Rendering


def verify(out):
    out = out.resolve()
    r = Rendering(out)
    palettes = {}
    for stage in (1, 3, 4, 5):
        scene = r.manifest['scenes'][stage - 1]
        blob = (out / f's{stage}.bin').read_bytes()
        first = scene['presentation']['hud'][0][0]
        offsets = [struct.unpack_from('<I', blob, scene['sprite_table'] + i * 16 + 8)[0]
                   for i in range(first, first + 26)]
        assert len(set(offsets)) == 1, ('HUD portraits and digits need one palette', stage, offsets)
        palettes[stage] = blob[offsets[0]:offsets[0] + 32]
    report = []
    with tempfile.TemporaryDirectory(prefix='power-hud-', dir=out) as base, \
            Emulator(out / 'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.dialogs(e)
        r.stage(e, 5)
        r.position(e, 248)
        r.field(e, 'powers', 2)
        e.run(20)
        for remaining in (1, 0):
            r.field(e, 'power_cd', 0)
            r.seed(e, 'safe_timer', 250, 1)
            e.input(4)
            e.run(3)
            e.input(0)
            r.until(e, lambda: r.state(e)['state'] == 5, step=1, limit=60)
            assert r.state(e)['powers'] == remaining, 'Select tap did not spend a power'
            r.until(e, lambda: r.state(e)['state'] != 5, step=1, limit=220)
            assert r.state(e)['state'] == 0, 'Power did not return to gameplay'
            for frame in range(90):
                actual = bytes.fromhex(e.call('asread', 'pram', 31 * 32, 32)['hex'])
                assert actual == palettes[5], ('Power left incorrect HUD colours', remaining, frame,
                                               actual.hex(), palettes[5].hex())
                e.run(1)
            e.screenshot(out / f'power-hud-{remaining}.png')
            report.append({'remaining_powers': remaining, 'hud_palette_correct_frames': 90})
        r.verify_sgx_code(e, 'power-hud')
    (out / 'power-hud-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/sgx'))
    verify(p.parse_args().out)
