#!/usr/bin/env python3
"""Seed the live hero into the hull cache; native boss arrival must preserve it.

Uses a fresh retail disc: old user savestates restore old application code.
Only cache placement is seeded during the native music seek. The CPU must
perform the handoff, hull upload and SAT publication itself.
"""
import argparse
import sys,struct,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
ap=argparse.ArgumentParser()
ap.add_argument('--out',type=Path,default=Path('build/pce'))
ap.add_argument('--review',type=Path,default=None)
args=ap.parse_args();out=args.out.resolve()
base=args.review or out/'boss-cache-handoff-review';base.mkdir(parents=True,exist_ok=True)
c=Campaign(out);elf=out/'app.elf';sym=lambda n:symbol(elf,n)
with Emulator(out/'saber_rider.cue',base) as e:
 boot(e,c.address);c.seed(e,'dialogs_done',255,1)
 cam=json.loads((out/'work/stage1.json').read_text())['width']-256
 c.move(e,cam+32,177);e.run(30)
 c.press(e,8);e.run(30)
 e.write(sym('actors'),bytes(8*21));e.write(sym('shots'),bytes(16*13));c.seed(e,'safe_timer',0,1)
 c.field(e,'boss_kind',1);c.seed(e,'boss_wait',1,1)
 e.input(8);e.run(1);e.input(0)
 c.until(e,lambda:e.memory(sym('boss_wait'),1)==b'\0',limit=90,step=1)
 assert e.memory(sym('hull_ready'),1)==b'\0', 'Must seed during the boss music seek'
 word=int.from_bytes(e.memory(sym('pce_sat_word'),2),'little')
 table=bytearray(bytes.fromhex(e.call('asread','sat0',0,512)['hex']))
 entries=list(struct.iter_unpack('<4H',table))
 hero=[k for k,s in enumerate(entries) if 120<=s[0]-64<210 and 0<=s[1]-32<256 and (s[3]&15)<15]
 assert hero,entries
 slot=entries[hero[0]][3]&15
 old=int.from_bytes(e.memory(sym('sprite_words')+slot*2,2),'little')
 owners=bytearray(e.memory(sym('pattern_owner'),48));count=sum(o==slot+1 for o in owners)*4;new=0x5800
 assert count and all(entries[k][3]&15==slot for k in hero)
 patterns=bytes.fromhex(e.call('asread','vram0',old*2,count*128)['hex']);e.write(new*2,patterns,'vram0')
 for k in range(48):
  if owners[k]==slot+1:owners[k]=0
 for k in range(16,16+(count+3)//4):owners[k]=slot+1
 e.write(sym('pattern_owner'),owners);e.write(sym('sprite_words')+slot*2,new.to_bytes(2,'little'))
 e.write(sym('sprite_pb_lo')+slot,bytes([(new>>5)&255]));e.write(sym('sprite_pb_hi')+slot,bytes([new>>13]))
 for k in hero:struct.pack_into('<H',table,k*8+4,entries[k][2]+((new-old)>>5))
 e.write(word*2,table,'vram0');e.write(0,table,'sat0')
 missing=[];trace=[]
 for f in range(90):
  e.run(1)
  sat=bytes.fromhex(e.call('asread','sat0',0,512)['hex'])
  hero_entries=[s for s in struct.iter_unpack('<4H',sat) if 120<=(s[0]&1023)-64<210 and 0<=(s[1]&1023)-32<256 and (s[3]&15)<15]
  if not hero_entries:missing.append(f)
  trace.append(dict(frame=f,hero_entries=len(hero_entries),hull_ready=e.memory(sym('hull_ready'),1)[0],hull_patterns=int.from_bytes(e.memory(sym('hull_patterns'),4),'little')))
  if f<35:e.screenshot(base/f'{f:03}.png')
 (base/'trace.json').write_text(json.dumps(trace,indent=2))
 print('Relocated hero slot',slot,'patterns',count,'missing hero frames',missing,flush=True)
 assert e.memory(sym('hull_ready'),1)==b'\1', 'Native boss arrival must complete'
 assert not missing,missing
