#!/usr/bin/env python3
"""Last-level hit-flash isolation, empty sprites and stale SAT retirement."""
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
    names = ('foes', 'bolts', 'foe_flash', 'event_index', 'event_count', 'pending',
             'sat_count', 'sprite_emit_id', 'sprite_emit_x',
             'sprite_emit_y', 'sprite_emit_flip', 'sprite_fast', 'sprite_emit_ok',
             'sprite_fast_miss')
    a = {n: symbol(out/'app.elf', n) for n in names}
    blob=(out/'s7.bin').read_bytes()
    palette_offset=struct.unpack_from('<I',blob,r.manifest['scenes'][6]['sprite_table']+5*16+8)[0]
    gunship_colours=blob[palette_offset:palette_offset+32]
    report = {}
    with tempfile.TemporaryDirectory(prefix='sgx-last4-', dir=out) as base, Emulator(
            out/'saber_rider.cue', base, sgx=True) as e:
        boot(e, r.address)
        r.dialogs(e)
        r.seed(e, 'stage', 6, 1)
        e.write(r.address+7, b'\6')
        r.field(e, 'state', 2)
        r.advance(e, 7)
        r.dialogs(e)
        r.seed(e, 'event_index', e.memory(a['event_count'], 1)[0], 1)
        r.seed(e, 'pending', 0, 1)
        r.seed(e, 'hurt', 255, 1)
        e.write(a['foes'], bytes(88));e.write(a['bolts'], bytes(96))
        # Two gunships share one cached pattern and palette, but only the
        # first is struck by this real player bolt on the next native tick.
        e.write(a['foes'], struct.pack('<hhBBBBHB', 180, 60, 5, 22, 0, 0, 1, 0)+
                struct.pack('<hhBBBBHB', 180, 150, 5, 22, 0, 0, 1, 0))
        r.settle(e)
        hud_before = bytes.fromhex(e.call('asread', 'pram', 31*32, 32)['hex'])
        x = int.from_bytes(e.memory(a['foes'], 2), 'little')
        e.write(a['bolts'], struct.pack('<hhbbBB', x-6, 60, 6, 0, 1, 0))
        white_seen = False
        for frame in range(30):
            e.run(1)
            slot = e.memory(r.sym['sprite_slot_of']+5, 1)[0]
            assert slot<48
            word = int.from_bytes(e.memory(r.sym['sprite_words']+slot*2, 2),'little')
            count = e.memory(r.sym['sprite_count']+slot,1)[0]
            entries = [t for vdc in (0,1) for t in r.sat(e,vdc)
                       if t[0] and word>>5 <= t[2] < (word+count*64)>>5]
            upper = [t for t in entries if t[0]<164]
            lower = [t for t in entries if t[0]>=164]
            assert upper and lower, ('fixture lost gunships', frame, entries)
            assert all(t[3]&15!=14 for t in lower), ('Unstruck gunship flashed', frame, lower)
            for palette in {t[3]&15 for t in lower}:
                actual=bytes.fromhex(e.call('asread','pram',(16+palette)*32,32)['hex'])
                assert actual==gunship_colours, ('Unstruck gunship palette changed',frame,palette,actual.hex())
            if any(t[3]&15==14 for t in upper):
                white_seen = True
                assert all(t[3]&15==14 for t in upper), ('Incomplete actor flash',upper)
                palette = struct.unpack('<16H',bytes.fromhex(e.call('asread','pram',30*32,32)['hex']))
                assert palette[1:]==(511,)*15, ('White palette overwritten',palette)
                e.screenshot(out/'last4-hit-flash.png')
            assert bytes.fromhex(e.call('asread','pram',31*32,32)['hex'])==hud_before, 'Shot changed HUD colours'
        assert white_seen, 'Native shot never produced a visible actor flash'
        assert e.memory(a['foe_flash'],8)==bytes(8), 'Actor hit timer did not expire'
        assert all(t[3]&15!=14 for vdc in (0,1) for t in r.sat(e,vdc) if t[0])
        report['hit_flash'] = {'same_type_actors':2, 'only_struck_actor_white':True,
                               'restored_after_expiry':True, 'hud_palette_unchanged':True}
        # Leave rogue text-like sprites in the last entry of BOTH alternating
        # DMA sources, outside their remembered active counts. Native drawing
        # must retire them on each VDC instead of preserving stale VRAM tails.
        rogue = struct.pack('<4H', 264, 64, 0x380, 0x8f)
        for vdc in (0,1):
            for word in (0x7e00,0x7f00):
                e.write(word*2+63*8, rogue, space=f'vram{vdc}')
        e.run(24)
        for vdc in (0,1):
            assert r.sat(e,vdc)[63][0]==0, ('Stale SAT tail survived',vdc)
            for word in (0x7e00,0x7f00):
                assert bytes.fromhex(e.call('asread',f'vram{vdc}',word*2+63*8,2)['hex'])==bytes(2)
        report['stale_sat_tails'] = {'vdc_count':2,'dma_sources_per_vdc':2,'retired':True}
        e.screenshot(out/'last4-clean-sat.png')
        r.verify_sgx_code(e,'last4')
        # The empty battle_cruiser record is a BG-only placeholder. Exercise
        # its compiled sprite path: it must neither allocate nor emit a piece.
        before = e.memory(a['sat_count'],1)
        e.write(a['sprite_emit_id'],b'\6\0')
        e.write(a['sprite_emit_x'],struct.pack('<h',64))
        e.write(a['sprite_emit_y'],struct.pack('<h',180))
        e.write(a['sprite_emit_flip'],b'\0')
        address=a['sprite_fast']
        e.write(0x3bf0,bytes((0x20,address&255,address>>8,0x4c,0xf3,0x3b)))
        for key,value in (('P',0),('SP',253),('MPR3',116),('MPR6',108),('PC',0x3bf0)):
            e.call('register_set',key,value)
        e.run(2)
        assert e.memory(a['sat_count'],1)==before, 'Empty placeholder emitted a sprite'
        assert e.memory(a['sprite_fast_miss'],1)==b'\0', 'Empty placeholder requested a cache upload'
        assert e.memory(r.sym['sprite_slot_of']+6,1)==b'\xff', 'Empty placeholder acquired a cache slot'
        report['empty_cruiser_sprite'] = {'no_sat_entries':True,'no_cache_upload':True}
    (out/'sgx-last4-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--out',type=Path,default=Path('build/sgx'))
    verify(p.parse_args().out)
