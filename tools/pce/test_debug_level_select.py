#!/usr/bin/env python3
"""Exercise DEBUG=1 level selection through native options and campaign flow."""
import argparse
import tempfile
import struct
from pathlib import Path
from emulator import Emulator, symbol
from test_campaign import Campaign


def verify(out, sgx=False):
    t = Campaign(out, sgx=sgx)
    ui = symbol(out / 'app.elf', 'pce_ui_state')
    level = symbol(out / 'app.elf', 'start_level')
    with tempfile.TemporaryDirectory(prefix='level-select-', dir=out) as base, Emulator(out / 'saber_rider.cue', base, sgx=sgx) as e:
        e.run(120); e.input(8); e.run(5); e.input(0)
        t.until(e, lambda: e.memory(t.address, 4) == b'SRPC' and e.memory(ui, 1) == b'\1', limit=20000)
        e.run(180)
        t.press(e, 64); t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\3')
        for _ in range(5): t.press(e, 64)
        assert e.memory(level, 1) == b'\1'
        t.press(e, 128)
        assert e.memory(level, 1) == b'\7', 'Left must wrap 1 to 7'
        t.press(e, 32)
        assert e.memory(level, 1) == b'\1', 'Right must wrap 7 to 1'
        for _ in range(2): t.press(e, 32)
        assert e.memory(level, 1) == b'\3'
        for expected in (4, 5, 6, 7, 1, 2, 3):
            t.press(e, 32)
            assert e.memory(level, 1)[0] == expected
        bat = bytes.fromhex(e.call('asread', 'vram0', (18 * 64 + 26) * 2, 4)['hex'])
        assert struct.unpack('<2H', bat) == (0xf090, 0xf093), ('Visible level digits', bat.hex())
        e.screenshot(out / 'debug-level-options.png')
        # Exit via the final row, return to options and verify persistence.
        t.press(e, 64); t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\1')
        t.press(e, 64); t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\3')
        assert e.memory(level, 1) == b'\3'
        t.press(e, 4)
        t.until(e, lambda: e.memory(ui, 1) == b'\1')
        t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\2')
        t.press(e, 1)
        t.until(e, lambda: t.metrics(e)['ready'] and e.memory(ui, 1) == b'\0', limit=20000)
        assert t.metrics(e)['stage'] == 3, 'New game must load the selected level'
        e.screenshot(out / 'debug-level-gameplay.png')
        # No continues: return through game over, then start another new game.
        t.seed(e, 'pce_continues', 0, 1); t.field(e, 'state', 3)
        t.until(e, lambda: e.memory(ui, 1) == b'\4', limit=20000)
        e.run(180); t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\1', limit=20000)
        t.press(e, 1)
        t.until(e, lambda: e.memory(ui, 1) == b'\2')
        t.press(e, 1)
        t.until(e, lambda: t.metrics(e)['ready'] and e.memory(ui, 1) == b'\0', limit=20000)
        assert t.metrics(e)['stage'] == 3, 'Game-over restart must honor the selected level'
    print('DEBUG level selection passed: wrap, persistence, new game and game-over restart', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/sgx'))
    p.add_argument('--sgx', action='store_true')
    args = p.parse_args()
    verify(args.out.resolve(), args.sgx)
