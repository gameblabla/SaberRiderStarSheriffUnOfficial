#!/usr/bin/env python3
"""Compare native VDC patterns with the exported foreground alpha mask."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out):
    t=Campaign(out)
    scene=json.loads((out/'manifest.json').read_text())['scenes'][0]
    blob=(out/'s1.bin').read_bytes();records=scene['records']
    mask=records['foreground_masks']['offset'];table=records['sprite_table']['offset']
    checked=changed=partial=0
    with tempfile.TemporaryDirectory(prefix='foreground-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,t.address)
        t.seed(e,'dialogs_done',255,1)
        t.press(e,8);e.run(120)
        e.write(symbol(out/'app.elf','trigger_remaining'),bytes(100))
        e.write(symbol(out/'app.elf','actors'),bytes(8*17))
        e.write(symbol(out/'app.elf','shots'),bytes(24*10))
        t.seed(e,'safe_timer',0,1)
        t.press(e,8)
        for world_x,direction in ((3048,0),(3032,128)):
            t.move(e,world_x,177)
            if direction:t.press(e,direction,3)
            e.run(120);t.capture(e,f'foreground-{direction}')
            t.press(e,8);e.run(120)
            d=t.metrics(e)
            ids=struct.unpack('<12H',e.memory(symbol(out/'app.elf','sprite_ids'),24))
            sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
            for k in range(d['sat_count']):
                sy,sx,pattern,attr=struct.unpack_from('<4H',sat,k*8)
                sprite=ids[attr&15]
                if sprite>=9:continue
                px,py=sx-32,sy-64;flip=bool(attr&0x800)
                origin_x=d['player_x']-d['camera_x'];origin_y=d['player_y']-16
                pat,parts,_pal,count=struct.unpack_from('<3IH',blob,table+sprite*16)
                match=[]
                for n in range(count):
                    dx,dy,index=struct.unpack_from('<2hH',blob,parts+n*6)
                    if flip:dx=-dx-16
                    if (dx,dy)==(px-origin_x,py-origin_y):match.append(index)
                assert len(match)==1,(sprite,px,py,d)
                raw=blob[pat+match[0]*128:pat+(match[0]+1)*128];expected=bytearray(raw)
                for row in range(16):
                    y=py+row
                    if not 0<=y<224:continue
                    for x in range(16):
                        world_x=d['camera_x']+px+x
                        if not 0<=world_x<scene['cols']*8:continue
                        alpha=blob[mask+scene['cols']*2+(world_x//8)*224+y]&(128>>(world_x&7))
                        if alpha:
                            bit=1<<(x if flip else 15-x)
                            for plane in range(4):
                                offset=plane*32+row*2
                                bits=int.from_bytes(expected[offset:offset+2],'little')&~bit
                                expected[offset:offset+2]=bits.to_bytes(2,'little')
                actual=bytes.fromhex(e.call('asread','vram0',pattern*64,128)['hex'])
                assert actual==expected,('Foreground pattern mismatch',direction,k)
                checked+=1;changed+=expected!=raw;partial+=expected!=raw and any(expected)
            t.press(e,8)
    assert checked>=4 and changed>=2 and partial>=2,(checked,changed,partial)
    result=dict(patterns_checked=checked,patterns_masked=changed,partial_patterns=partial,facings=2)
    (out/'foreground-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Native foreground pattern checks passed:',result)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True)
    verify(p.parse_args().out.resolve())
