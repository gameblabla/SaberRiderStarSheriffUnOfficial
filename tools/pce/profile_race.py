#!/usr/bin/env python3
"""Measure road completion rate with the live seven-car field and with it removed."""
import sys,tempfile,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
import argparse
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));p.add_argument('--sgx',action='store_true');p.add_argument('--tag',default='current');args=p.parse_args();out=args.out.resolve();c=Campaign(out,sgx=args.sgx);reports=[]
with tempfile.TemporaryDirectory(dir=out) as b,Emulator(out/'saber_rider.cue',b,sgx=args.sgx) as e:
 boot(e,c.address);c.seed(e,'stage',1,1);c.field(e,'state',2);c.advance(e,2);c.dialogs(e);e.run(240)
 e.input(16);e.run(180)
 for kind in ('rivals','empty'):
  if kind=='empty': e.write(symbol(out/'app.elf','rv'),bytes(7*23))
  before=c.metrics(e);e.call('prof_start');e.run(300);after=c.metrics(e)
  path=out/f'race-{args.tag}-{kind}-cycles.txt';e.call('prof_dump',str(path))
  reports.append(dict(field=kind,video_frames=300,road_updates=after['floor_commits']-before['floor_commits'],road_updates_per_second=(after['floor_commits']-before['floor_commits'])/5))
  print(reports[-1],flush=True)
  e.screenshot(out/f'race-{args.tag}-{kind}.png')

(out/f'race-{args.tag}-profile.json').write_text(json.dumps(reports,indent=2)+'\n')
