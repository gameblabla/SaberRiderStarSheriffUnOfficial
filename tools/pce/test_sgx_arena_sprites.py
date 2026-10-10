#!/usr/bin/env python3
"""Keep rear robot patterns intact during punches; grow approaching cacti."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering


def read(e, space, address, size):
    return bytes.fromhex(e.call('asread', space, address, size)['hex'])


def verify(out, save=None):
    out = out.resolve()
    r = Rendering(out)
    blob = (out/'s6.bin').read_bytes()
    scene = r.manifest['scenes'][5]
    columns = [set(struct.unpack_from('<H', blob, scene['map']+x*90+y*3)[0]
                   for y in range(30)) for x in range(scene['cols'])]
    peak = max(len(set().union(*(columns[(x+k)%len(columns)] for k in range(33))))
               for x in range(len(columns)))
    assert peak<=512-4-32, ('arena background exceeds runtime cache budget', peak)
    report = {'background_peak_tiles': peak}
    with tempfile.TemporaryDirectory(prefix='arena-sprites-', dir=out) as base, Emulator(out/'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.stage(e, 6)
        arena = 108*8192 + (symbol(out/'app.elf', 'trigger_cache') & 8191)
        aim = struct.unpack('<h', e.memory(arena+505, 2, logical=False))[0]

        def mech(angle, distance, variant):
            return struct.pack('<3h3H11B', angle, 500, 0, distance, 60000, 850,
                               4, variant, 200, 0, 0, 0, 0, 0, 0, 0, 0)

        # Native stationary aiming robots, with the red one behind BG0.
        e.write(arena, mech((aim-640)%21504, 1200, 0)+mech((aim+640)%21504, 1664, 1)+bytes(23+16*17+10*9), space='physical')
        e.write(arena+524, bytes((4, 0, 0, 0, 0, 0)), space='physical')
        e.write(arena+531, bytes((250,)), space='physical')
        r.field(e, 'wave', 2)
        e.run(60)
        table = scene['records']['m6_big_table']['offset']
        big_keys = 121*8192 + (symbol(out/'app_full.elf', 'big_key') & 8191)
        checks = 0
        for keys in (0, 2, 0, 2, 0):
            e.input(keys)
            for _ in range(60):
                e.write(arena+14, bytes((250,)), space='physical')
                e.write(arena+23+14, bytes((250,)), space='physical')
                e.run(1)
                rear = [row for row in r.sat(e, 1) if row[0] and row[3]&15 == 14]
                assert rear, 'fixture must display a full-size rear robot'
                word = 0x3000 if rear[0][2] < 0x340 else 0x6800
                key = struct.unpack('<H', e.memory(big_keys+(0 if word==0x6800 else 2), 2, logical=False))[0]
                assert 48<=key<52, ('rear aiming robot pose', key)
                patterns, count = struct.unpack_from('<IB', blob, table+key*10+4)
                expected = blob[patterns:patterns+count*512]
                actual = read(e, 'vram1', word*2, count*512)
                if actual != expected:
                    e.screenshot(out/'arena-pattern-failure.png')
                assert actual == expected, ('punch corrupted rear robot patterns', checks, hex(word))
                checks += 1
        e.input(0)
        e.screenshot(out/'arena-punch-patterns.png')
        report['rear_robot_pattern_frames'] = checks

        # The reported save has detached sprites in the unused SAT tail.
        # Poison both inactive DMA sources: count-only transfers otherwise
        # leave these entries intact when that source is published again.
        ghost = struct.pack('<4H', 220, 240, 0x180, 0x8e)
        for vdc in (0, 1):
            for word in (0x7e00, 0x7f00):
                e.write(word*2+63*8, ghost, space=f'vram{vdc}')
        e.run(30)
        for vdc in (0, 1):
            assert r.sat(e, vdc)[63][0] == 0, ('stale arena SAT tail became visible', vdc)
            for word in (0x7e00, 0x7f00):
                assert read(e, f'vram{vdc}', word*2+63*8, 2) == bytes(2), ('stale SAT source tail', vdc, hex(word))
        report['stale_tail_sources_cleared'] = 4

        # Remove robots/shots/FX and keep wave completion blocked by its
        # remaining spawn count. Only the seeded cactus enters the world SAT.
        e.write(arena, bytes(3*23+16*17+10*9), space='physical')
        e.write(arena+524, bytes((0, 0, 0, 0, 0, 0)), space='physical')
        e.write(arena+515, bytes(2), space='physical')
        r.field(e, 'wave', 0)
        cactus = [i for i, s in enumerate(scene['sprites']) if s['name'].startswith('cactus_')]
        heights = []
        for distance in (1200, 600, 300, 160, 80):
            props = struct.pack('<hHB', aim, distance*4, 2) + struct.pack('<hHB', (aim+10752)%21504, 6400, 0)*13
            e.write(arena+431, props, space='physical')
            e.write(arena+515, bytes(2), space='physical')
            e.run(30)
            r.settle(e)
            active = []
            for sprite_id in cactus:
                slot = e.memory(r.sym['sprite_slot_of']+sprite_id, 1)[0]
                if slot>=48 or struct.unpack('<H', e.memory(r.sym['sprite_ids']+slot*2, 2))[0]!=sprite_id:
                    continue
                word = struct.unpack('<H', e.memory(r.sym['sprite_words']+slot*2, 2))[0]
                count = e.memory(r.sym['sprite_count']+slot, 1)[0]
                if any(row[0] and word>>5<=row[2]<(word>>5)+count*2 for vdc in (0,1) for row in r.sat(e,vdc)):
                    active.append(sprite_id)
            assert len(active)==1, ('must display exactly one cactus size', distance, active)
            height = scene['sprites'][active[0]]['height']
            heights.append(height)
            e.screenshot(out/f'arena-cactus-{distance}.png')
        assert heights == sorted(heights), heights
        assert heights[-1]>=90 and heights[-1]>=heights[1]*3, ('close cactus must keep growing', heights)
        report['cactus_heights_far_to_near'] = heights
        if save:
            from test_sgx_arena_saves import sections
            saved_ram = sections(save)['HuC']['SysCardRAM']
            first_bank = 0x68 if len(saved_ram)==24*8192 else 0x50
            start = (108-first_bank)*8192+(arena&8191)
            # Retain fresh runtime/compositor metadata after the old game's
            # 570-byte arena state; loading the entire save restores old code.
            e.write(arena, saved_ram[start:start+570], space='physical')
            r.field(e, 'wave', 1)
            for _ in range(360):
                e.run(1)
                for vdc in (0, 1):
                    assert all(not row[0] or 0<row[0]<288 for row in r.sat(e,vdc)), ('rogue offscreen sprite Y', vdc)
            e.screenshot(out/'arena-reported-save-replay.png')
            report['reported_gameplay_replay_frames'] = 360
        r.verify_sgx_code(e, 'arena punches and cactus scales')
        assert e.memory(r.sgx_address+8, 1) == bytes(1), 'SGX transfers must succeed'
        assert e.memory(r.address+20, 4) == bytes(4), 'disc reads during rendering/load errors'
    (out/'arena-sprites-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    parser.add_argument('--save', type=Path, help='Replay old arena gameplay data on fresh code')
    args = parser.parse_args()
    verify(args.out, args.save)
