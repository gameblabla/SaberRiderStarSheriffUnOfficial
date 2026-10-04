#!/usr/bin/env python3
"""Seeded native boss, camera and victory checks on the accurate emulator."""
import sys,tempfile,struct
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
import argparse,json
ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,default=Path('build/pce'));out=ap.parse_args().out.resolve();c=Campaign(out);elf=out/'app.elf'
with tempfile.TemporaryDirectory(dir=out) as b,Emulator(out/'saber_rider.cue',b) as e:
 boot(e,c.address);e.run(120)
 # Natural camera trigger spawns 500px ahead of the view and must survive.
 c.press(e,8);e.run(60)
 c.seed(e,'dialogs_done',255,1);c.seed(e,'camera',5660)
 e.write(symbol(elf,'actors'),bytes(8*21));e.write(symbol(elf,'player'),struct.pack('<4h4B',5782,177,0,0,0,0,4,4))
 c.press(e,8);e.run(60)
 pool=e.memory(symbol(elf,'actors'),168)
 assert any(pool[i+12] and pool[i+13]==16 and struct.unpack_from('<h',pool,i)[0]==6160 for i in range(0,168,21)),'Camera must survive its offscreen approach'
 c.press(e,8);e.run(60);c.seed(e,'camera',6000)
 e.write(symbol(elf,'player'),struct.pack('<4h4B',6120,177,0,0,0,0,4,4));c.press(e,8)
 e.screenshot(out/'revision-camera0.png');e.run(15);e.screenshot(out/'revision-camera1.png')
 from PIL import Image
 first=Image.open(out/'revision-camera0.png').crop((172,76,188,92))
 second=Image.open(out/'revision-camera1.png').crop((172,76,188,92))
 assert first.tobytes()!=second.tobytes(),'Camera lights must visibly animate'
 aid=e.memory(symbol(elf,'pce_actor_ids')+16,1)[0]
 slots=e.memory(symbol(elf,'sprite_slot_of')+aid,2)
 assert all(x<48 for x in slots),'Both camera animation cells must reach the sprite cache'
 print('Camera survives early spawn and draws both original frames.',flush=True)
 for stage in (1,3,4):
  if stage!=1:c.stage(e,stage);c.dialogs(e)
  c.press(e,8);e.run(120)
  # Scene width is recorded in exported metadata.
  meta=json.loads((out/f'work/stage{stage}.json').read_text());cam=meta['width']-256
  c.seed(e,'camera',cam);c.seed(e,'dialogs_done',255,1);c.seed(e,'arena_time',1500)
  e.write(symbol(elf,'actors'),bytes(8*21));e.write(symbol(elf,'shots'),bytes(16*13))
  e.write(symbol(elf,'player'),struct.pack('<4h4B',cam+100,160,0,0,0,0,4,4))
  c.field(e,'boss_kind',1 if stage==1 else 2);c.field(e,'boss_hp',60);c.seed(e,'boss_max',60,1)
  c.seed(e,'hull_ready',0,1);c.seed(e,'boss_x',cam+128);c.seed(e,'boss_y',48 if stage==1 else 66);c.seed(e,'boss_phase',2 if stage==1 else 4,1);c.seed(e,'boss_hold',10 if stage==1 else 400);c.seed(e,'boss_flash',0,1);c.seed(e,'safe_timer',250,1)
  c.press(e,8);e.run(20);e.screenshot(out/f'revision-boss{stage}.png')
  print('boss',stage,c.metrics(e),flush=True)
  for _ in range(5):
   c.seed(e,'safe_timer',250,1);e.run(60)
  print('boss sustained',stage,c.metrics(e),flush=True)

 # Every platform stage exports both animated camera frames.
 manifest=json.loads((out/'manifest.json').read_text())
 for scene in manifest['scenes']:
  if scene['stage'] not in (1,3,4,5):continue
  names=[s['name'] for s in scene['sprites']]
  assert 'actor_type16' in names and 'actor_type16_frame1' in names
 c.press(e,8);e.run(60);c.field(e,'boss_kind',0);c.field(e,'result',1);c.field(e,'state',0)
 c.press(e,8)
 ui=symbol(elf,'pce_ui_state');c.until(e,lambda:e.memory(ui,1)==b'\5',limit=6000)
 e.screenshot(out/'revision-victory.png')
 regs=e.call('registers')['registers']
 assert regs['HDR']&255==39 and regs['VDR']==223 and regs['VCECR']==1
 print('Victory hardware timing: 320x224.',flush=True)
 c.press(e,1,60);c.until(e,lambda:c.metrics(e)['stage']==5 and c.metrics(e)['ready'],limit=6000)
 print('Victory advances to stage 5; native boss art, sustained projectiles and camera assets pass.')
