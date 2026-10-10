#!/usr/bin/env python3
"""Check the stage-7 planet, power cut-in and native BAT restoration."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering


def verify(out):
    out = out.resolve()
    r = Rendering(out)
    names = ('space_frame', 'power_frame', 'space_near_sgx_bat_body', 'pce_control.6',
             'flight_clock', 'pce_sky_near', 'space_hull_ready')
    a = {name: symbol(out / 'app.elf', name) for name in names}
    scene = r.manifest['scenes'][6]
    blob = (out / 's7.bin').read_bytes()
    patterns, mapping, _, _ = struct.unpack_from(
        '<IIHB', blob, scene['records']['sgx_near_record']['offset'])
    original = struct.unpack_from('<2048H', blob, mapping)
    blank = 0x80
    samples = []
    power_samples = []
    with tempfile.TemporaryDirectory(prefix='sgx-planet-', dir=out) as base, \
            Emulator(out / 'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.dialogs(e)
        e.run(120)
        # Stage 6 advances directly to 7, bypassing the victory screen.
        # Seed both stage owners so the transition helper takes that path.
        r.seed(e, 'stage', 6, 1)
        e.write(r.address + 7, b'\6')
        r.field(e, 'state', 2)
        r.advance(e, 7)
        r.dialogs(e)
        e.run(120)
        assert e.memory(a['space_hull_ready'], 1) == b'\0'
        # Freeze gameplay ticks, then call the compiled draw with IRQs running.
        # This reaches late scroll phases without triggering the cruiser first.
        e.write(a['pce_control.6'], b'\0')

        def invoke(name, bank):
            address = a[name]
            e.write(0x3bf0, bytes((0x20, address & 255, address >> 8,
                                 0x4c, 0xf3, 0x3b)))
            for key, value in (('P', 0), ('SP', 253), ('MPR3', bank),
                               ('MPR6', 108), ('PC', 0x3bf0)):
                e.call('register_set', key, value)
            for _ in range(120):
                e.run(1)
                if e.call('registers')['registers']['PC'] in (0x3bf3, 0x3bf4, 0x3bf5):
                    return
            raise AssertionError(('Native call did not return', name))

        def check(scroll):
            e.run(2)  # Let VBlank publish the new scroll on both VDCs.
            # The RPC uses duplicate names for the two VDC register groups.
            # Preserve those pairs so the planet's VDC0 value is checked too.
            e.proc.stdin.write('registers\n')
            e.proc.stdin.flush()
            while True:
                line = e.proc.stdout.readline()
                if line.startswith('{'):
                    registers = dict(json.loads(line, object_pairs_hook=list))['registers']
                    break
            bxr = [value for name, value in registers if name == 'BXR']
            assert bxr[0] == scroll, ('planet inherited nebula scroll', scroll, bxr)
            actual_scroll = int.from_bytes(e.memory(a['pce_sky_near'], 2), 'little')
            assert actual_scroll == scroll, ('planet scroll', actual_scroll, scroll)
            cells = struct.unpack('<2048H', bytes.fromhex(
                e.call('asread', 'vram0', 0, 4096)['hex']))
            retired = min(scroll // 8, 33)
            vram = bytes.fromhex(e.call('asread', 'vram0', 0, 65536)['hex'])
            assert vram[blank * 32:(blank + 1) * 32] == bytes(32)
            ink = 0
            for world in range(scroll // 8, (scroll + 255) // 8 + 1):
                for y in range(28):
                    cell = cells[y * 64 + (world & 63)]
                    expected = original[y * 64 + world] if world < 64 else blank
                    assert cell == expected, ('wrapped planet returned', scroll, world, y)
                    tile = (cell & 4095) - blank
                    data = vram[(cell & 4095) * 32:((cell & 4095) + 1) * 32]
                    assert data == blob[patterns + tile * 32:patterns + (tile + 1) * 32]
                    ink += bool(any(data))
            if scroll >= 264:
                assert ink == 0, ('planet visible after leaving', scroll, ink)
            elif scroll <= 128:
                assert ink > 0, ('planet disappeared before leaving', scroll)
            return {'scroll': scroll, 'retired_columns': retired, 'visible_ink_tiles': ink}

        def power(scroll):
            def read(space, address, size):
                return bytes.fromhex(e.call('asread', space, address, size)['hex'])

            bat = read('vram0', 0, 4096)
            font = read('vram0', 0x4200 * 2, 3072)
            sky = read('vram1', 0, 65536)
            palette = read('pram', 0, 1024)
            r.field(e, 'state', 5)
            r.field(e, 'timer', 0)
            for t in range(105):
                invoke('power_frame', 123)
                if t == 40:
                    cells = struct.unpack('<2048H', read('vram0', 0, 4096))
                    for row in range(14):
                        kind = 6 if row == 0 else 7 if row == 13 else row % 6
                        for x in range(33):
                            col = ((scroll >> 3) + x) & 63
                            expected = 0xf400 + kind * 8 + (col & 7)
                            assert cells[(7 + row) * 64 + col] == expected, (
                                'power wave missing from displayed planet window', scroll, row, x)
                    e.screenshot(out / f'space-power-{scroll}.png')
            assert r.state(e)['state'] == 0
            assert read('vram0', 0, 4096) == bat, ('power left tiles in planet BAT', scroll)
            assert read('vram0', 0x4200 * 2, 3072) == font, ('power font restore', scroll)
            assert read('vram1', 0, 65536) == sky, ('power changed nebula', scroll)
            # Palette 15 retains the animated wave ramp for the white-out;
            # the planet and nebula use palettes 0..9.
            restored = read('pram', 0, 1024)
            assert restored[:10 * 32] == palette[:10 * 32], ('space background palettes', scroll)
            check(scroll)
            e.screenshot(out / f'space-power-{scroll}-restored.png')
            power_samples.append({'scroll': scroll, 'visible_wave_cells': 33 * 14,
                                  'planet_bat_restored': True, 'font_restored': True,
                                  'nebula_unchanged': True, 'background_palettes_restored': True})

        # Keep the initial natural drift, then cross pixel and BAT wrap edges.
        start = int.from_bytes(e.memory(a['pce_sky_near'], 2), 'little')
        for scroll in (start, 8, 128, 248, 255, 256, 257, 259, 260, 263, 264, 512, 1024):
            e.write(a['flight_clock'], struct.pack('<H', scroll * 32))
            invoke('space_frame', 120)
            samples.append(check(min(scroll, 264)))
            if scroll in (128, 257, 264):
                power(min(scroll, 264))
            if scroll in (start, 128, 257, 264, 512):
                e.screenshot(out / f'space-planet-{scroll}.png')
        # The same native BAT restore is used after dialogue and power panels.
        invoke('space_near_sgx_bat_body', 113)
        check(264)
        e.write(a['flight_clock'], bytes(2))
        invoke('space_frame', 120)
        check(264)
        r.verify_sgx_code(e, 'planet scroll and restore')
        r.metrics(e)
    report = {'samples': samples, 'power_samples': power_samples,
              'restore_kept_space': True, 'clock_wrap_kept_space': True}
    (out / 'sgx-space-planet-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/debug-sgx'))
    verify(parser.parse_args().out)
