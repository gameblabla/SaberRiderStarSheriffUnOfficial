#!/usr/bin/env python3
"""Seed a walking/firing fight; measure displayed SAT generations and CPU cycles."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def run(out,label):
    out=out.resolve();c=Campaign(out,sgx=True);elf=out/'app.elf'
    names=('player','actors','shots','pce_presented','pce_draws')
    sy={n:symbol(elf,n) for n in names}
    read=lambda e,n:int.from_bytes(e.memory(sy[n],2),'little')
    with tempfile.TemporaryDirectory(prefix='combat-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);e.run(120);c.seed(e,'dialogs_done',255,1)
        c.move(e,600);c.press(e,8);e.run(120)
        e.write(sy['shots'],bytes(208))
        rows=[]
        for i in range(6):
            rows.append(struct.pack('<4h4B9B',640+i*28,160,0,0,0,0,4,4,1,[1,2,5,6,7,9][i],20,i*4,1,0,i*3,0,0))
        e.write(sy['actors'],b''.join(rows)+bytes(42));c.seed(e,'safe_timer',250,1)
        c.press(e,8);e.input(33)
        before=read(e,'pce_presented');m=c.metrics(e);e.call('prof_start')
        samples=[];actors=[]
        for i in range(180):
            c.seed(e,'safe_timer',250,1)
            e.input(1|(32 if (i//30)%2==0 else 128));old=read(e,'pce_presented');e.run(1)
            samples.append((read(e,'pce_presented')-old)&65535)
            actors.append(sum(e.memory(sy['actors'],168)[12::21]))
        log=out/f'combat-{label}-cycles.txt';e.call('prof_dump',str(log))
        e.screenshot(out/f'combat-{label}.png')
        report=dict(video_frames=180,presented=(read(e,'pce_presented')-before)&65535,
            fps=round(sum(samples)/3,2),active_actors_min=min(actors),active_actors_max=max(actors),
            upload_bytes=(c.metrics(e)['uploads']-m['uploads'])&65535,
            overflow=c.metrics(e)['essential_overflow']-m['essential_overflow'])
        (out/f'combat-{label}.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
        assert max(actors)>=4 and min(actors)>=3,'Must exercise multiple native enemies'
        if label=='after':assert report['fps']>=32,'Combat cadence regressed below the optimized fixture floor'
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));p.add_argument('--label',default='after')
    a=p.parse_args();run(a.out,a.label)
