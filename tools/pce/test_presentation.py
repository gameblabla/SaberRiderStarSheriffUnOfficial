#!/usr/bin/env python3
"""Exercise actual title/selection/briefing inputs and every hero's diagonal SAT."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
import numpy as np
from PIL import Image
from emulator import Emulator,symbol
from test_campaign import Campaign

def verify(out):
    sprites=json.load(open(out/'manifest.json'))['scenes'][0]['sprites']
    t=Campaign(out);ui=symbol(out/'app.elf','pce_ui_state');report={}
    captures=out/'presentation-review';captures.mkdir(exist_ok=True)
    def picture(e,name):
        p=captures/(name+'.png');e.screenshot(p)
        im=np.asarray(Image.open(p).convert('RGB'))
        assert len(np.unique(im.reshape(-1,3),axis=0))>16,(name,'Blank screen')
    with tempfile.TemporaryDirectory(prefix='presentation-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        e.run(120);e.input(8);e.run(5);e.input(0)
        t.until(e,lambda:e.memory(ui,1)==b'\1');e.run(360);picture(e,'title')
        t.press(e,1);e.run(240);assert e.memory(ui,1)==b'\2';picture(e,'select-saber')
        for hero in (1,2,3):
            t.press(e,32);e.run(240);picture(e,f'select-{hero}')
        t.press(e,1);e.run(240);assert e.memory(ui,1)==b'\3';picture(e,'briefing')
        for _ in range(30):
            if e.memory(ui,1)==b'\0':break
            t.press(e,1)
        t.until(e,lambda:e.memory(t.address+5,1)==b'\1');e.run(120)
        assert t.metrics(e)['hero']==3,'Selected Colt must reach actual gameplay'
        report['frontend']='title -> all four selections -> briefing -> Colt gameplay'
        # Use the existing Run menu to change hero without rebuilding test ROMs.
        for hero in range(4):
            t.press(e,8);current=t.metrics(e)['hero']
            for _ in range((hero-current)&3):t.press(e,16)
            t.press(e,8);t.until(e,lambda:e.memory(t.address+5,1)==b'\1');e.run(120)
            t.seed(e,'dialogs_done',255,1);t.field(e,'state',0)
            picture(e,f'hero-{hero}-hud')
            reads=t.metrics(e)['disc_reads']
            for direction,keys in [('up-right',52),('down-right',100),('up-left',148),('down-left',196)]:
                e.input(keys);e.run(30);picture(e,f'hero-{hero}-{direction}')
                sat=bytes.fromhex(e.call('asread','vram0',0xfe00,512)['hex'])
                ids=struct.unpack('<48H',e.memory(symbol(out/'app.elf','sprite_ids'),96))
                found=[]
                for k in range(t.metrics(e)['sat_count']):
                    y,x,pat,attr=struct.unpack_from('<4H',sat,k*8)
                    if attr&15>=15:continue
                    name=sprites[ids[attr&15]]['name']
                    if name==f'hero{hero}_'+('up_right' if direction.startswith('up') else 'down_right'):
                        found.append(bool(attr&0x800))
                assert found and all(v==direction.endswith('left') for v in found),(hero,direction,found)
                assert e.call('registers')['registers']['BYR']==0
                e.input(0);e.run(30)
            assert t.metrics(e)['disc_reads']==reads,'Aim/HUD must use preloaded graphics'
        report['aim_cases']=16;report['hud_heroes']=4;report['platform_vertical_scroll']=0
    (out/'presentation-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));a=p.parse_args();verify(a.out.resolve())
