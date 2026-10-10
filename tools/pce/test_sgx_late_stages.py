#!/usr/bin/env python3
"""Full-width platform BAT/pattern sweeps and late-stage native rendering checks."""
import argparse,tempfile,struct,json,sys,numpy as np
from pathlib import Path
sys.path.append(str(Path(__file__).resolve().parents[1]/"saturn"))
from test_sgx_rendering import Rendering
from emulator import Emulator,boot,symbol
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'))
out=p.parse_args().out.resolve();c=Rendering(out);report={}
with tempfile.TemporaryDirectory(dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
 # Independently protect the stage-4 raster-band holes: source sky pixels
 # outside mountain ink must remain opaque in the baked lower band.
 import levl
 from test_port import decode_tile
 level,files=levl.load_dump(out/'work/stage4.layers')
 banks={i:levl.png_bank(path,i) if path else levl.load_bank(out/'work/srgb'/f'{i:08X}.srgb') for i,path in files.items()}
 layers={ly.name:ly for ly in level.layers}
 sky=layers['SkyBG'];mountains=layers['FarMountains']
 source_sky=levl.render_layer(sky,banks[sky.cblock],levl.layer_offset(sky,0),sky.w*banks[sky.cblock].tw,240,None)
 source_near=levl.render_layer(mountains,banks[mountains.cblock],0,mountains.w*banks[mountains.cblock].tw,240,None)
 covered=np.tile(source_sky[...,3],(1,(source_near.shape[1]+source_sky.shape[1]-1)//source_sky.shape[1]))[:,:source_near.shape[1]]>=128
 holes=covered&(source_near[...,3]<128);holes[:64]=False
 scene=c.manifest['scenes'][3];blob=(out/'s4.bin').read_bytes()
 record=struct.unpack_from('<IIHBIHIHHHHBIHHHBII',blob,scene['records']['sgx_sky_record']['offset'])
 assert record[10]==64 and record[12]==0 and record[16]==0,('Stage 4 must bake one quarter-speed sky record',record[10],record[12],record[16])
 checked=0
 for x in range(record[9]):
  for row in range(8,30):
   mask=holes[row*8:row*8+8,x*8:x*8+8]
   if not mask.any():continue
   id=struct.unpack_from('<H',blob,record[1]+x*90+row*3)[0]
   pixels=decode_tile(blob[record[0]+id*32:record[0]+(id+1)*32])
   assert (pixels[mask]>0).all(),('Stage 4 missing sky behind mountain band',x,row)
   checked+=int(mask.sum())
 assert checked>10000,('Must exercise source sky holes',checked)
 report['stage4_source_sky_hole_pixels']=checked
 boot(e,c.address)
 for st in (4,5):
  c.stage(e,st);s=c.manifest['scenes'][st-1];blob=(out/f's{st}.bin').read_bytes();checks=[]
  for x in range(160,s['width']-256,192):
   c.position(e,x);c.settle(e);c.sky(e)
   ids=struct.unpack('<896H',e.memory(symbol(out/'app.elf','cache_ids'),1792));vram=bytes.fromhex(e.call('asread','vram0',0,65536)['hex']);cam=c.metrics(e)['camera_x']
   for col in range(cam//8,cam//8+33):
    for row in range(28):
     id,pal=struct.unpack_from('<HB',blob,s['sgx_map']+(col%s['cols'])*90+row*3);word=struct.unpack_from('<H',vram,(row*64+(col&63))*2)[0];slot=(word&4095)-128
     assert 0<=slot<896 and ids[slot]==id,(st,cam,col,row,'BAT id',slot,ids[slot] if 0<=slot<896 else None,id)
     assert word>>12==pal,(st,cam,col,row,'palette')
     assert vram[(word&4095)*32:(word&4095)*32+32]==blob[s['sgx_tiles']+id*32:s['sgx_tiles']+(id+1)*32],(st,cam,col,row,'patterns')
   checks.append(cam)
  print(st,checks,flush=True);report[str(st)]=checks
 for st in (6,7):
  c.stage(e,st);e.run(120);e.input(1)
  before=c.word(e,'pce_presented');metrics=c.metrics(e);samples=[]
  for frame in range(180):
   c.seed(e,'safe_timer',250,1);old=c.word(e,'pce_presented');e.run(1)
   samples.append((c.word(e,'pce_presented')-old)&65535)
  e.input(0);e.screenshot(out/f'late-stage{st}.png')
  entries=0;peaks=[]
  for vdc in (0,1):
   lines=[0]*224
   for y,x,pattern,attr in c.sat(e,vdc):
    if not y:continue
    entries+=1;y=(y&1023)-64;h=(16,32,64,64)[(attr>>12)&3]
    for row in range(max(0,y),min(224,y+h)):lines[row]+=2 if attr&256 else 1
   peaks.append(max(lines))
  assert entries and max(peaks)<=16,('Late-stage native sprite budgets',st,entries,peaks)
  report[str(st)]=dict(video_frames=180,presentations=sum(samples),fps=round(sum(samples)/3,2),sprite_entries=entries,peak_units=peaks,
      essential_overflow=c.metrics(e)['essential_overflow']-metrics['essential_overflow'])
  print(st,report[str(st)],flush=True)
(out/'late-stage-sweep.json').write_text(json.dumps(report,indent=2))
