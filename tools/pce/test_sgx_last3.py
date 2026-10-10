#!/usr/bin/env python3
"""Native forest parallax, arena dialogue, and two full-size robot checks."""
import argparse
import json
import struct
import tempfile
from pathlib import Path
from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering


def read(e, space, address, size):
    return bytes.fromhex(e.call('asread', space, address, size)['hex'])


def verify(out):
    out = out.resolve()
    capture = Path('/tmp/sgx-dialogue-luna-high')
    capture.mkdir(parents=True, exist_ok=True)
    r = Rendering(out)
    report = {}
    with tempfile.TemporaryDirectory(prefix='sgx-last3-', dir=capture) as base, Emulator(out/'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.dialogs(e)
        r.stage(e, 4)
        samples = []
        for camera in (128, 510, 518, 1022, 2054):
            r.position(e, camera+120)
            r.settle(e)
            r.sky(e)
            scroll = r.word(e, 'pce_sgx_sky_scroll_x')
            assert scroll == (camera+3)//4 and scroll < camera
            samples.append({'camera': camera, 'sky': scroll})
        report['forest_parallax'] = samples
        r.seed(e, 'stage', 5, 1)
        r.field(e, 'state', 2)
        r.advance(e, 6)
        assert r.state(e)['state'] == 1
        e.run(120)
        vram = read(e, 'vram0', 0, 65536)
        cells = struct.unpack('<2048H', vram[:4096])
        assert e.memory(symbol(out/'app.elf', 'story_column'), 2) == bytes(2)
        for row in range(3, 9):
            for x in range(5, 29):
                assert cells[row*64+x] >> 12 == 15, ('panel screen cell', row, x, cells[row*64+x])
        assert sum(0xf421 <= cells[row*64+x] < 0xf480 for row in range(4, 8) for x in range(5, 29)) > 20
        e.screenshot(capture/'ramrod-dialog-after.png')
        report['arena_dialogue_screen_cells'] = 144
        # Retire only the real cached dialogue-corner patterns and check every
        # frame until the panel cells return to the arena background.
        elf=out/'app.elf'
        dialog=r.manifest['scenes'][5]['presentation']['dialog']
        def corner_ranges_now():
            cache_ids=struct.unpack('<48H',e.memory(r.sym['sprite_ids'],96))
            words=struct.unpack('<48H',e.memory(r.sym['sprite_words'],96))
            counts=e.memory(r.sym['sprite_count'],48)
            return [(words[slot]>>5,(words[slot]+counts[slot]*64)>>5)
                    for slot,sprite_id in enumerate(cache_ids) if dialog<=sprite_id<dialog+8]
        def corner_entries(ranges):
            return [entry for entry in r.sat(e,0) if entry[0] and
                    any(lo<=entry[2]<hi for lo,hi in ranges)]
        corner_ranges=corner_ranges_now()
        corners=corner_entries(corner_ranges)
        assert corners, 'Arena dialogue did not publish its corner patterns'
        pages=e.memory(symbol(elf,'page_count'),1)[0]
        final_corner_ranges=[]
        for page in range(pages):
            e.run(360)
            assert r.state(e)['state']==1 and r.state(e)['page']==page
            page_ranges=corner_ranges_now()
            page_corners=corner_entries(page_ranges)
            assert page_corners, ('Arena corners missing on settled page',page)
            if page+1==pages:
                final_corner_ranges=page_ranges
                assert len(page_corners)==4, ('Final arena dialogue should have four corner SAT entries',page_corners)
            if page+1<pages:
                e.input(1)
                for frame in range(1,121):
                    e.run(1)
                    if r.state(e)['page']==page+1:break
                else:raise AssertionError(('Arena story page did not advance',page))
                e.input(0);e.run(1)
        e.input(1)
        restored_frame=None
        for frame in range(1,121):
            e.run(1)
            sat=r.sat(e,0)
            vram=read(e,'vram0',0,4096)
            bat=struct.unpack('<2048H',vram)
            panel_left=any(bat[row*64+x]>>12==15 for row in range(3,9) for x in range(5,29))
            live_corners=[entry for entry in sat if entry[0] and any(
                lo<=entry[2]<hi for lo,hi in final_corner_ranges)]
            if not panel_left:
                assert not live_corners, ('Arena dialogue corners remained when panel BAT was restored',frame,live_corners)
                restored_frame=frame
            if r.state(e)['state']!=1 and restored_frame is not None:break
        else:raise AssertionError(('Arena panel did not restore after story close',r.state(e),restored_frame))
        e.input(0)
        assert restored_frame is not None, 'Arena panel BAT did not restore after story close'
        assert not corner_entries(final_corner_ranges), 'Arena dialogue corner entries remained after close'
        report['arena_dialogue_restore']={'pages':pages,'corner_entries':len(corners),'restored_frame':restored_frame}
        r.press(e, 8)
        arena = 108*8192 + (symbol(out/'app.elf', 'trigger_cache') & 8191)
        aim = struct.unpack('<h', e.memory(arena+505, 2, logical=False))[0]
        def mech(angle, distance, variant):
            return struct.pack('<3h3H11B', angle, 500, 0, distance, 600, 850,
                               4, variant, 200, 0, 0, 0, 0, 0, 0, 0, 0)
        e.write(arena, mech((aim-320)%21504, 1200, 0)+mech((aim+320)%21504, 1600, 1)+bytes(23+16*17+10*9), space="physical")
        e.write(arena+524, bytes((4, 0, 0, 0, 0, 0)), space="physical")
        e.write(arena+531, bytes((250,)), space="physical")
        r.field(e, 'wave', 2)
        r.press(e, 8)
        e.run(30)
        r.settle(e)
        e.screenshot(capture/'ramrod-two-large-after.png')
        assert e.memory(arena+602, 1, logical=False) == bytes((1,)), 'nearest full-size robot must use BG0'
        rear = [entry for entry in r.sat(e, 1) if entry[0] and entry[3]&15 == 14]
        assert rear and all(entry[3]&0x1100 == 0x1100 for entry in rear), 'second robot must use full-size 32x32 sprites on VDC1'
        assert not [entry for entry in r.sat(e, 0) if entry[0] and entry[3]&15 == 14], 'rear robot must stay below nearest BG robot'
        # Match actual sprite pattern storage to the selected baked big pose.
        blob = (out/'s6.bin').read_bytes()
        table = r.manifest['scenes'][5]['records']['m6_big_table']['offset']
        patterns, count = struct.unpack_from('<IB', blob, table+48*10+4)
        word = 0x3000 if rear[0][2] < 0x340 else 0x6800
        assert read(e, 'vram1', word*2, count*512) == blob[patterns:patterns+count*512], 'second robot patterns must match its full-size pose'
        e.screenshot(capture/'ramrod-two-large-after.png')
        report['two_large_robots'] = {'nearest': 'BG0', 'second': 'VDC1 sprites', 'second_sprite_entries': len(rear), 'pattern_bytes': count*512}
        r.verify_sgx_code(e, 'two large robots')
        r.metrics(e)
        # Reopen a native story after the robot compositor has used both BAT
        # halves, then require its screen-space panel and clean return to play.
        r.field(e, 'story', 1)
        r.field(e, 'event', 1)
        r.until(e, lambda: r.state(e)['state'] == 1, step=1, limit=120)
        e.run(120)
        cells = struct.unpack('<2048H', read(e, 'vram0', 0, 4096))
        assert sum(0xf421 <= cells[row*64+x] < 0xf480 for row in range(4, 8) for x in range(5, 29)) > 20
        assert e.memory(r.sgx_address+5, 1)[0] & 8 == 0, 'dialogue must use BAT half zero'
        e.screenshot(capture/'ramrod-late-dialog-after.png')
        r.dialogs(e)
        r.until(e, lambda: e.memory(arena+602, 1, logical=False) == bytes((1,)), step=1, limit=120)
        report['late_dialogue_and_robot_restore'] = True
    (capture/'sgx-last3-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/debug-sgx'))
    verify(parser.parse_args().out)
