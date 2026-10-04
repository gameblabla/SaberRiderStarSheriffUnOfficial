#!/usr/bin/env python3
"""Placed enemies (kneelers, shield snipers), the shield's behaviour and the sprite admission order on the platform stages."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def actors(t,e):
    raw=e.memory(symbol(t.out/'app.elf','actors'),8*21);rows=[]
    for k in range(8):
        r=raw[k*21:(k+1)*21];x,y=struct.unpack_from('<2h',r,0)
        active,kind,hp,timer,flip,dead,anim,aim,mode=r[12:21]
        rows.append(dict(active=active,type=kind,x=x,y=y,hp=hp,flip=flip,dead=dead,aim=aim,mode=mode))
    return [a for a in rows if a['active']]

def verify(out):
    t=Campaign(out);elf=out/'app.elf'
    scenes=json.loads((out/'manifest.json').read_text())['scenes']
    assert all(s['foreground_count']==0 for s in (scenes[0],scenes[2]))
    results={}
    with tempfile.TemporaryDirectory(prefix='priority-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,t.address);e.run(120)
        # A placed kneeler stands 280 px beyond the narrow zone that wakes it: it must be there, and stay, when the hero walks up.
        t.move(e,1300,100);t.seed(e,'safe_timer',250,1);e.input(32)
        seen=False
        for _ in range(12):
            e.run(20)
            if any(a['type']==8 for a in actors(t,e)):seen=True;break
        e.input(0);assert seen,'The kneeler placed at x 1620 must spawn when its zone is crossed'
        k=[a for a in actors(t,e) if a['type']==8][0];assert k['x']>=1600,k
        e.run(120);assert any(a['type']==8 for a in actors(t,e)),'A placed enemy must not be culled while ahead of the hero'
        results['kneeler']=k
        # Stage 4: the shield sniper takes front shots on its shield (hp of them), burns it away, then dies to one more hit.
        t.stage(e,4);t.dialogs(e);t.seed(e,'safe_timer',255,1)
        t.move(e,780,160);e.input(32);shield=None
        for _ in range(30):
            e.run(20);found=[a for a in actors(t,e) if a['type']==30]
            if found:shield=found[0];break
        e.input(0);assert shield,'The shield sniper placed at x 1080 must spawn'
        hp=shield['hp'];assert hp in (4,6,8)
        t.move(e,1010,160);t.seed(e,'safe_timer',255,1);e.run(150)   # the camera follows 4 px a step: the shield must be on screen for shots to count
        slot=next(k for k in range(8) if (lambda r:r[12]==1 and r[13]==30)(e.memory(symbol(elf,'actors')+k*21,21)))
        base_address=symbol(elf,'actors')+slot*21
        def hit(from_front=True):
            raw=e.memory(base_address,21);x,y=struct.unpack_from('<2h',raw,0)
            data=bytearray(13*16);data[:10]=struct.pack('<4h2B',x+(-4 if raw[16] else 4),y-18,0,0,1,0)
            t.seed(e,'safe_timer',255,1);e.write(symbol(elf,'shots'),bytes(data));e.run(4)
        # Park the hero facing it so every shot is a front shot.
        raw=e.memory(base_address,21);x=struct.unpack_from('<h',raw,0)[0]
        for n in range(hp):
            e.write(symbol(elf,'facing'),bytes([0 if raw[16] else 1]));hit();raw=e.memory(base_address,21)
            if n<hp-1:assert raw[12]==1 and raw[20]&4==0 and raw[14]==hp-1-n,('the shield must absorb the shot',n,list(raw))
        raw=e.memory(base_address,21);assert raw[12]==1 and raw[20]&4 and raw[19]>0,('the shield must burn away, not die',list(raw))
        e.run(30);e.write(symbol(elf,'facing'),bytes([0 if raw[16] else 1]));hit();raw=e.memory(base_address,21)
        assert raw[17] or not raw[12],('one more hit kills the bare sniper',list(raw))
        results['shield']=dict(hp=hp)
        # Pressure from refused sprites decays when nothing is refused.
        e.write(symbol(elf,'enemy_pressure'),bytes([40]));e.write(symbol(elf,'shot_pressure'),bytes([30]));e.run(120)
        assert e.memory(symbol(elf,'enemy_pressure'),1)==b'\0' and e.memory(symbol(elf,'shot_pressure'),1)==b'\0','sprite-pressure counters must decay'
    (out/'priority-verification.json').write_text(json.dumps(results,indent=2)+'\n');print(results)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
