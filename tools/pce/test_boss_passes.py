#!/usr/bin/env python3
"""The gunship comes in from the distance: a far hull, a mid hull, then the full one; its engine, guns and wreck have sounds."""
import sys,tempfile,struct,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
import argparse
ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,default=Path('build/pce'));out=ap.parse_args().out.resolve();c=Campaign(out);elf=out/'app.elf'
audio=json.loads((out/'audio.json').read_text())
for voice in audio['voices']:
 events={s['event'] for s in voice['samples']}
 assert {'boss_appear','boss_cannon','boss_rider','boss_volley','boss_blast','boss_down'}<=events,voice['hero']
 assert voice['bytes']<=65536,'ADPCM RAM holds 64 KiB'
with tempfile.TemporaryDirectory(dir=out) as b,Emulator(out/'saber_rider.cue',b) as e:
 boot(e,c.address);e.run(120);c.seed(e,'dialogs_done',255,1)
 c.move(e,9800);c.until(e,lambda:c.state(e)['boss_kind']==1,limit=600)
 level=symbol(elf,'hull_level');phase=symbol(elf,'boss_phase')
 seen=[];playing=0
 for _ in range(60):
  e.run(12)
  value=e.memory(level,1)[0]
  if not seen or seen[-1]!=value:seen.append(value)
  playing+=e.call('registers')['registers']['Playing']
 assert seen[:3]==[2,1,0],f'hull sizes by pass: {seen}'
 assert e.memory(phase,1)[0]==2,'the fight begins after the passes'
 assert playing,'the engine pass and the guns must be heard (CD ADPCM)'
 print('Gunship hulls by pass',seen,'; ADPCM active in',playing,'of 60 samples.')
