#!/usr/bin/env python3
"""Seed each convoy approach; native triggers, scrolling and audio run normally."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out):
    c=Campaign(out);reports=[]
    meta=json.loads((out/'work/stage1.json').read_text())
    triggers=[t for t in meta['triggers'] if t['type']==11]
    stops=meta['stops']
    with tempfile.TemporaryDirectory(prefix='herd-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,c.address);e.run(120)
        on=symbol(out/'app.elf','herd_on');locked=symbol(out/'app.elf','herd_locked')
        voices=symbol(out/'app.elf','pce_pcm_voices')
        for i,(trigger,stop) in enumerate(zip(triggers,stops)):
            c.press(e,8);e.run(120)
            x=trigger['zone'][0]-40
            e.write(symbol(out/'app.elf','player'),struct.pack('<4h4B',x,160,0,0,0,0,4,4))
            c.seed(e,'camera',x-120)
            e.write(symbol(out/'app.elf','actors'),bytes(8*19))
            c.seed(e,'safe_timer',250,1);c.seed(e,'dialogs_done',255,1)
            c.press(e,8);e.input(32)
            c.until(e,lambda:e.memory(on,1)==b'\1',limit=600,step=1)
            start=c.metrics(e);assert e.memory(locked,1)==b'\0',start
            c.until(e,lambda:e.memory(locked,1)==b'\1',limit=1000,step=1)
            lock=c.metrics(e)
            assert lock['camera_x']>start['camera_x']+60,(start,lock)
            assert stop[0]-stop[2]-12<=lock['player_x']<=stop[0]+stop[2]+12,(stop,lock)
            e.input(0);e.run(30)
            assert e.memory(voices+16,2)!=b'\0\0','Gallop must play while herd lives'
            frozen=c.metrics(e)['camera_x'];c.capture(e,f'herd-{i+1}-locked')
            reads=c.metrics(e)['disc_reads']
            for _ in range(80):
                c.seed(e,'safe_timer',250,1);e.run(30)
                if e.memory(on,1)==b'\0':break
                assert c.metrics(e)['camera_x']==frozen
            assert e.memory(on,1)==e.memory(locked,1)==b'\0'
            assert e.memory(voices+16,2)==b'\0\0'
            assert c.metrics(e)['disc_reads']==reads
            e.input(32);e.run(60);e.input(0)
            assert c.metrics(e)['camera_x']>frozen
            reports.append(dict(trigger=trigger['zone'][0],stop=stop[0],spawn_camera=start['camera_x'],locked_camera=frozen,passed=True))
    (out/'herd-verification.json').write_text(json.dumps(reports,indent=2)+'\n');print(reports)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
