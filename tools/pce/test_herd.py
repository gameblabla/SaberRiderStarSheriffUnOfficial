#!/usr/bin/env python3
"""Seed each convoy approach; native triggers, scrolling and audio run normally."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
from test_sgx_rendering import Rendering

def verify(out,sgx=False):
    c=Campaign(out,sgx=sgx);reports=[]
    render=Rendering(out) if sgx else None
    meta=json.loads((out/'work/stage1.json').read_text())
    triggers=[t for t in meta['triggers'] if t['type']==11]
    stops=meta['stops']
    with tempfile.TemporaryDirectory(prefix='herd-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=sgx) as e:
        boot(e,c.address);e.run(120)
        on=symbol(out/'app.elf','herd_on');locked=symbol(out/'app.elf','herd_locked')
        voices=symbol(out/'app.elf','pce_pcm_voices')
        if sgx:
            scene=json.loads((out/'manifest.json').read_text())['scenes'][0]
            blob=(out/'s1.bin').read_bytes();offset=scene['horse_offset']
            bases=(320,400,480,800,880)
            front_poses=set();front_checks=0
            def check_horse_pages(e,label):
                nonlocal front_checks
                small=bytes.fromhex(e.call('asread','vram0',0x6400*2,5*1536)['hex'])
                assert small==blob[offset+32+5*5120:offset+32+5*(5120+1536)],('Second-row poses overwritten',label)
                front=list(struct.iter_unpack('<4H',bytes.fromhex(e.call('asread','sat0',0,512)['hex'])))
                horses=[s for s in front if s[0] and (s[3]&15)==13]
                for y,x,p,a in horses:
                    pose,part=divmod(p-0x6400//32,24)
                    assert 0<=pose<5 and part in (0,8,16,20),('Second-row pattern',label,p)
                    assert ((a>>12)&3)==(1 if part<16 else 0) and a&256,('Second-row size',label,a)
                    front_poses.add(pose);front_checks+=1
                lines=[0]*224
                for y,x,p,a in front:
                    if not y:continue
                    y=(y&1023)-64;h=(16,32,64,64)[(a>>12)&3]
                    for row in range(max(0,y),min(224,y+h)):lines[row]+=2 if a&256 else 1
                assert max(lines)<=16,('Second-row scanline overflow',label,max(lines))
                for pose,base in enumerate(bases):
                    data=bytes.fromhex(e.call('asread','vram1',base*64,5120)['hex'])
                    assert data==blob[offset+32+pose*5120:offset+32+(pose+1)*5120],('Herd pose overwritten',label,pose)
                sat_now=[s for s in struct.iter_unpack('<4H',bytes.fromhex(e.call('asread','sat1',0,512)['hex'])) if s[0]]
                protected=[(base,base+80) for base in bases]
                assert all(any(lo<=s[2]<hi for lo,hi in protected) == ((s[3]&15)==13)
                           for s in sat_now),('non-herd VDC1 sprite uses a reserved horse page',label,sat_now)
                horse_cells=[s for s in sat_now if (s[3]&15)==13]
                top_patterns={base+16*c for base in bases for c in range(4)}
                bottom_patterns={base+64+4*c for base in bases for c in range(4)}
                allowed=top_patterns|bottom_patterns
                assert all((s[3]&15)==13 and s[2] in allowed and
                           ((s[3]>>12)&3) in (0,2,3) for s in horse_cells),('horse SAT palette/pattern/size',label,horse_cells)
                assert all(((s[3]>>12)&3) in (2,3) if s[2] in top_patterns else ((s[3]>>12)&3)==0
                           for s in horse_cells),('horse SAT cell size does not match pose region',label,horse_cells)
                assert all(0<=s[1]<512 and 0<=s[0]<1024 for s in horse_cells),('horse SAT coordinates',label,horse_cells)
        for i,(trigger,stop) in enumerate(zip(triggers,stops)):
            c.press(e,8);e.run(120)
            if sgx:
                # All five horse poses are loaded at scene start. Their VDC1
                # pages must already be resident before any convoy is drawn.
                check_horse_pages(e,'before convoy')
            x=trigger['zone'][0]-40
            e.write(symbol(out/'app.elf','player'),struct.pack('<4h4B',x,160,0,0,0,0,4,4))
            c.seed(e,'camera',x-120)
            e.write(symbol(out/'app.elf','actors'),bytes(8*21))
            c.seed(e,'safe_timer',250,1);c.seed(e,'dialogs_done',255,1)
            c.press(e,8);e.input(32)
            # The hero stays invulnerable on the run to the lock: being trampled on the one frame where the horses
            # fill a scanline is a separate (phase dependent) rendering case, not what this test measures.
            def reached(flag):
                def done():
                    c.seed(e,'safe_timer',250,1);return e.memory(flag,1)==b'\1'
                return done
            c.until(e,reached(on),limit=600,step=1)
            start=c.metrics(e);assert e.memory(locked,1)==b'\0',start
            c.until(e,reached(locked),limit=1000,step=1)
            # The simulation sets herd_locked before play_tick refreshes
            # telemetry. Let that simulation step finish before reading
            # player_x; the previous sample can be one movement step behind.
            e.run(4)
            lock=c.metrics(e)
            assert lock['camera_x']>start['camera_x']+60,(start,lock)
            assert stop[0]-stop[2]-12<=lock['player_x']<=stop[0]+stop[2]+12,(stop,lock)
            e.input(0);e.run(30)
            assert e.memory(voices+16,2)!=b'\0\0','Gallop must play while herd lives'
            frozen=c.metrics(e)['camera_x'];c.capture(e,f'herd-{i+1}-locked')
            if sgx:
                sky_scroll=symbol(out/'app.elf','pce_sgx_sky_scroll_x')
                locked_sky=int.from_bytes(e.memory(sky_scroll,2),'little')
                palette=bytes.fromhex(e.call('asread','pram',29*32,32)['hex'])
                assert palette==blob[offset:offset+32],('herd palette',palette.hex())
                # Wide hero/actor cells share the horse width bit. Palette 13
                # identifies horses; check_horse_pages also verifies ownership.
                check_horse_pages(e,'locked convoy')
                cells=[s for s in struct.iter_unpack('<4H',bytes.fromhex(e.call('asread','sat1',0,512)['hex'])) if s[0] and (s[3]&15)==13]
                assert cells, 'No horses rendered in locked convoy'

            reads=c.metrics(e)['disc_reads']
            horse_poses=set()
            presented=symbol(out/'app.elf','pce_presented');draw0=int.from_bytes(e.memory(presented,2),'little');frames_run=0
            if sgx:
                # Fire while the locked convoy fills the second VDC's SAT.
                # This exercises actor allocation and horse admission together.
                e.input(1);c.capture(e,f'herd-{i+1}-locked-firing')
            fg_checks=fg_parts=hero_bullet_checks=0;shake_values=set();background_shakes=set()
            for pressure_sample in range(80):
                step=31 if sgx else 30
                c.seed(e,'safe_timer',250,1);e.run(step);frames_run+=step
                if e.memory(on,1)==b'\0':break
                assert c.metrics(e)['camera_x']==frozen
                if sgx:
                    background_shakes.add(int.from_bytes(e.memory(symbol(out/'app.elf','pce_sat_scroll_y'),2),'little'))
                    assert int.from_bytes(e.memory(sky_scroll,2),'little')==locked_sky,('sky moved while player/camera locked',i)
                    # All five resident poses must remain intact throughout
                    # the convoy, including while hardware SAT DMA runs.
                    check_horse_pages(e,'during convoy')
                    live=[s for s in struct.iter_unpack('<4H',bytes.fromhex(e.call('asread','sat1',0,512)['hex'])) if s[0] and (s[3]&15)==13]
                    horse_poses.update(bases.index(max(b for b in bases if s[2]>=b)) for s in live)
                    # Under continuous fire, every foreground part still
                    # retained for the current camera must remain in VDC0's
                    # published SAT. Also prove that the hero's live shot is
                    # admitted alongside the convoy.
                    if pressure_sample % 8 == 0:
                        retained=render.foreground(e,allow_herd_shake=True)
                        fg_checks+=1;fg_parts+=retained
                        shake_values.add(render.last_fg_shake)
                        assert render.last_fg_shake==0,'Foreground shook with the background'
                        shot_pool=e.memory(render.sym['shots'],208)
                        hero_live=any(struct.unpack_from('<4h5B',shot_pool,k*13)[4] and
                                      struct.unpack_from('<4h5B',shot_pool,k*13)[5]==0
                                      for k in range(16))
                        if hero_live:
                            assert render.sprite_entries(e,36),('live hero bullet missing under convoy pressure',i,pressure_sample)
                            hero_bullet_checks+=1
            fps=((int.from_bytes(e.memory(presented,2),'little')-draw0)&65535)*60/frames_run
            e.input(0)
            assert e.memory(on,1)==e.memory(locked,1)==b'\0'
            assert e.memory(voices+16,2)==b'\0\0'
            assert c.metrics(e)['disc_reads']==reads
            if sgx:
                assert horse_poses==set(range(5)),('herd animation',horse_poses)
                assert front_poses==set(range(5)) and front_checks>0,('Second-row animation',front_poses,front_checks)
                assert len(background_shakes)>1,('Background shake did not vary',background_shakes)
            if sgx and i==1:
                assert fg_checks>=5 and fg_parts>0,('no retained foreground pressure samples',fg_checks,fg_parts)
                assert hero_bullet_checks>0,('hero projectile never coexisted with convoy foreground',hero_bullet_checks)
            if sgx:check_horse_pages(e,'after convoy')
            # Include continuous firing during camera catch-up after the herd:
            # the right edge must remain loaded and SAT widths must still fit.
            e.input(32|1);e.run(60);e.input(0)
            assert c.metrics(e)['camera_x']>frozen
            scroll=int.from_bytes(e.memory(symbol(out/'app.elf','pce_scroll_x'),2),'little')
            columns=struct.unpack('<990H',e.memory(symbol(out/'app.elf','columns'),1980))
            bat=struct.unpack('<2048H',bytes.fromhex(e.call('asread','vram0',0,4096)['hex']))
            for world in range((scroll>>3)+31,(scroll>>3)+33):
                for row in range(30):
                    assert bat[row*64+(world&63)]&4095==128+columns[(world%33)*30+row],('right edge',i,world,row)
            sat=bytes.fromhex(e.call('asread','sat0',0,512)['hex'])
            lines=[0]*224
            for y,x,pattern,attr in struct.iter_unpack('<4H',sat):
                if not y:continue
                y=(y&1023)-64;h=(16,32,64,64)[(attr>>12)&3]
                for line in range(max(y,0),min(y+h,224)):lines[line]+=2 if attr&0x100 else 1
            assert max(lines)<=16
            c.capture(e,f'herd-{i+1}-after')
            reports.append(dict(trigger=trigger['zone'][0],stop=stop[0],spawn_camera=start['camera_x'],locked_camera=frozen,
                                post_herd_peak=max(lines),right_edge_cells=60,herd_fps=round(fps,2),
                                foreground_pressure_checks=fg_checks,foreground_retained_parts=fg_parts,
                                foreground_shake_values=sorted(shake_values),background_shake_values=sorted(background_shakes),
                                hero_bullet_pressure_checks=hero_bullet_checks,passed=True))
    (out/'herd-verification.json').write_text(json.dumps(reports,indent=2)+'\n');print(reports)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));p.add_argument('--sgx',action='store_true');a=p.parse_args();verify(a.out.resolve(),a.sgx)
