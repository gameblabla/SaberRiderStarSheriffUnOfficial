#!/usr/bin/env python3
"""Verify foreground SAT priority and unchanged actor patterns in both facings."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out):
    t=Campaign(out)
    scenes=json.loads((out/'manifest.json').read_text())['scenes'];scene=scenes[4]
    assert all(scenes[k]['foreground_count']==0 for k in (0,2)),'stages 1 and 3 have no foreground'
    assert scenes[3]['foreground_count']>0,'stage 4 keeps its cabin walls'
    blob=(out/'s5.bin').read_bytes();table=scene['records']['sprite_table']['offset']
    fg_table=scene['records']['foreground_sprites']['offset'];fg_x=[struct.unpack_from('<hhH',blob,fg_table+6*k)[0] for k in range(scene['foreground_count'])]
    # Stage 5 currently has no source foreground. Still check actor patterns and
    # the empty-layer path; retain the priority assertions for future scenery.
    mid=fg_x[len(fg_x)//2] if fg_x else 400
    first=next((i for i,s in enumerate(scene['sprites']) if s['name'].startswith('foreground_')),None)
    checked=foreground=0
    with tempfile.TemporaryDirectory(prefix='foreground-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,t.address);t.stage(e,5);t.dialogs(e);t.seed(e,'dialogs_done',255,1)
        t.press(e,8);e.run(120)
        e.write(symbol(out/'app.elf','trigger_remaining'),bytes(100))
        e.write(symbol(out/'app.elf','actors'),bytes(8*21));e.write(symbol(out/'app.elf','shots'),bytes(16*13))
        t.seed(e,'safe_timer',0,1);t.press(e,8)
        for world_x,direction in ((mid+40,0),(mid+24,128)):
            t.move(e,world_x,150)
            t.press(e,8);e.run(60);t.seed(e,'camera',world_x-120);t.press(e,8)   # the camera only closes 4 px a step
            if direction:t.press(e,direction,3)
            e.run(120);t.capture(e,f'foreground-{direction}');t.press(e,8);e.run(120)
            d=t.metrics(e)
            ids=struct.unpack('<48H',e.memory(symbol(out/'app.elf','sprite_ids'),96))
            words=struct.unpack('<48H',e.memory(symbol(out/'app.elf','sprite_words'),96))
            sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
            hero_entries=[];fg_entries=[]
            for k in range(d['sat_count']):
                sy,sx,pattern,attr=struct.unpack_from('<4H',sat,k*8)
                if attr&15<15:slot=attr&15
                else:
                    candidates=[]
                    for slot in range(15,48):
                        if ids[slot]==65535:continue
                        count=struct.unpack_from('<H',blob,table+ids[slot]*16+12)[0]
                        if words[slot]<=pattern*32<words[slot]+count*64:candidates.append(slot)
                    assert len(candidates)==1,(pattern,candidates)
                    slot=candidates[0]
                sprite=ids[slot];pat,parts,_pal,count=struct.unpack_from('<3IH',blob,table+sprite*16)
                piece=(pattern*32-words[slot])//64
                assert 0<=piece<count
                expected=blob[pat+piece*128:pat+(piece+1)*128]
                actual=bytes.fromhex(e.call('asread','vram0',pattern*64,128)['hex'])
                assert actual==expected,'Foreground must use SAT priority, never cut actor patterns'
                if scene['sprites'][sprite]['name'].startswith('hero'):
                    hero_entries.append(k);assert bool(attr&0x800)==bool(direction);checked+=1
                if first is not None and sprite>=first:fg_entries.append(k);foreground+=1
            assert hero_entries,'Capture must include the hero'
            if fg_x:
                assert fg_entries,'Capture must include foreground scenery'
                assert max(fg_entries)<min(hero_entries),'Earlier SAT entries cover later actors'
            else:
                assert not fg_entries and first is None,'Empty foreground must allocate no scenery sprites'
            assert e.call('registers')['registers']['BYR']==0
            t.press(e,8)
    report=dict(checked_patterns=checked,foreground_entries=foreground,facings=2,
                actor_patterns='unmodified',foreground_priority='passed' if fg_x else 'empty source layer',vertical_scroll=0)
    (out/'foreground-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));a=p.parse_args();verify(a.out.resolve())
