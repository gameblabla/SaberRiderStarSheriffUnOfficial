#!/usr/bin/env python3
"""SGX race sky restoration and rendered perspective-size regression checks.

The lifecycle uses native race/story transitions with a seeded final lap.
The distance sweep invokes the compiled draw path with fixed world positions.
"""
import argparse,json,struct,tempfile,wave
from PIL import Image
import numpy as np
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering
from test_sgx_reported_issues import read_space
from test_road import autopilot,tables

def verify(out):
    out=out.resolve();c=Rendering(out);elf=out/'app.elf';report={}
    scene=c.manifest['scenes'][1];blob=(out/'s2.bin').read_bytes()
    def sky(e):
        ids=struct.unpack('<256H',e.memory(symbol(elf,'cache_ids'),512))
        slots={id:i for i,id in enumerate(ids) if id!=65535}
        expected=[]
        for row in range(6):
            words=[]
            for x in range(6,62):
                id,pal=struct.unpack_from('<HB',blob,scene['map']+x*90+(row+3)*3)
                words.append(0x300+slots[id]|pal<<12)
            expected.append(struct.pack('<56H',*words))
        return b''.join(expected)
    def restored(e,name):
        actual=b''.join(read_space(e,'vram0',((51+row)*128+6)*2,112) for row in range(6))
        assert actual==sky(e),(name,'race sky differs from original asset')
        e.screenshot(out/f'race-{name}.png')
        report.setdefault('restored_dialogues',[]).append(name)
    with tempfile.TemporaryDirectory(prefix='sgx-race-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address)
        try:c.stage(e,2)
        except Exception:
            e.screenshot(out/'race-failure.png')
            print(e.call('registers'),flush=True)
            raise
        restored(e,'opening')
        # Exercise the real starting field before removing rivals for the
        # lifecycle fixture. Short zero-page output pointers previously wrote
        # video registers and left bogus depths/rows in this native list.
        grid=[]
        for frames,keys in ((0,0),(180,0),(120,16),(120,16|32),(120,16)):
            e.input(keys);e.run(frames)
            n=e.memory(symbol(elf,'nvis'),1)[0]
            points=list(struct.iter_unpack('<hhHB',e.memory(symbol(elf,'vis'),n*7)))
            for x,y,depth,kind in points:
                assert 40<=depth<=560 and y==113+10080//depth,('Invalid native projection',points)
            registers=e.call('registers')['registers']
            assert registers['WIND0']==registers['WIND1']==0,('Projection changed VPC windows',registers)
            rivals=[v for v in points if 1<=v[3]<=5]
            rendered=set()
            for id in range(9,39):
                entries=c.sprite_entries(e,id)
                if entries:rendered.add(id)
            if not grid:
                assert len(rivals)>=3,('Starting rivals not projected',points)
                assert rendered,('Starting rivals not rendered',points)
            grid.append(dict(frames=frames,keys=keys,rivals=len(rivals),sprites=sorted(rendered)))
            e.screenshot(out/f'race-grid-{len(grid)-1}.png')
        e.input(0);report['native_field']=grid
        driving=[]
        for second in range(12):
            for _ in range(12):
                e.input(autopilot(e,out,None));e.run(5)
                n=e.memory(symbol(elf,'nvis'),1)[0]
                points=list(struct.iter_unpack('<hhHB',e.memory(symbol(elf,'vis'),n*7)))
                assert all(40<=f<=560 and y==113+10080//f for x,y,f,kind in points),points
                bxr,selectors=tables(e,out)
                assert all(0<=s<=5 for s in selectors),('Corrupt road copy selector',selectors)
                registers=e.call('registers')['registers']
                assert registers['WIND0']==registers['WIND1']==0,('Projection changed VPC windows',registers)
            driving.append(dict(second=second+1,bxr=bxr[::12],visible=n))
            if second in (0,5,11):e.screenshot(out/f'race-driving-{second+1}.png')
        e.input(0);report['native_driving']=driving
        # Clear the rivals and seed only the last lap. Native code reaches the
        # finish, opens every page, then starts the straight pursuit.
        e.write(symbol(elf,'rv'),bytes(7*23));c.seed(e,'lapp',3,1)
        e.input(16);c.until(e,lambda:c.state(e)['state']==1,limit=3000);e.input(0)
        assert c.state(e)['story']==1
        assert read_space(e,'acram',0x1e0000,672)==sky(e),'Finish backup corrupted'
        c.dialogs(e);e.run(60);restored(e,'finish')
        assert e.memory(symbol(elf,'rphase'),1)==b'\3'
        bx,by=struct.unpack('<2h',e.memory(symbol(elf,'boss')+8,4))
        c.seed(e,'px',bx);c.seed(e,'py',(by+120)&8191)
        e.input(16);c.until(e,lambda:c.state(e)['state']==1,limit=1500);e.input(0)
        assert c.state(e)['story']==2
        c.dialogs(e);e.run(180);restored(e,'boss')
        def boss_audio(label):
            assert e.memory(symbol(elf,'pce_music_status'),1)==b'\0','Boss CD-DA request failed'
            assert e.memory(symbol(elf,'start_track'),1)==b'\x19','Boss must select logical track 17'
            path=out/f'race-boss-{label}.wav'
            e.call('sound_capture',120,str(path))
            with wave.open(str(path)) as w:
                pcm=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(float)
            rms=float(np.sqrt(np.mean(pcm*pcm)))
            assert rms>100,('Silent boss music',label,rms)
            report.setdefault('boss_music',{})[label]=rms
        boss_audio('transition')
        # Restoring the boss phase must request its track even if no edge
        # notification survived; the phase owns music until victory.
        c.seed(e,'boss_music_started',0,1)
        c.seed(e,'rphase',4,1);e.run(180)
        boss_audio('restored-phase')
        # Native victory dialogue closes before returning to the clear screen.
        c.seed(e,'rphase',5,1);c.seed(e,'phase_t',119)
        c.until(e,lambda:c.state(e)['state']==1,limit=600)
        c.dialogs(e);restored(e,'victory')

    # Victory hands Arcade RAM over to its painting. Use a fresh race archive
    # for the isolated renderer sweep, rather than drawing through that handoff.
    with tempfile.TemporaryDirectory(prefix='sgx-race-depth-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);c.stage(e,2)
        # Isolate the actual renderer, leaving hardware IRQs enabled for its
        # SAT/palette publication. No simulation moves the fixed test car.
        c.field(e,'result',0);c.field(e,'state',0);c.field(e,'event',0)
        c.seed(e,'rphase',3,1);c.seed(e,'hurt',0,1);c.seed(e,'tilt',0)
        c.seed(e,'spin',0,1);c.seed(e,'boost_on',0,1)
        c.seed(e,'cam_c',0,1);c.seed(e,'cam_s',-127,1)
        c.seed(e,'cam_cl',0,1);c.seed(e,'cam_sl',0,1)
        c.seed(e,'pce_control.0',4096);c.seed(e,'pce_control.1',4096)
        e.write(symbol(elf,'mines'),bytes(6*5));e.write(symbol(elf,'blasts'),bytes(4*5))
        e.write(symbol(elf,'race_bolts'),bytes(10*10));e.write(symbol(elf,'escort'),bytes(2*26))
        e.write(symbol(elf,'boss')+18,b'\3')
        fn=symbol(elf,'race_draw');sweep=[]
        for distance in (540,420,320,250,200,160,125,100,80,60,45):
            rival=struct.pack('<Hb3hH6B3h',0,0,0,0,0,0,1,0,1,0,0,0,4096,4096-distance,0)
            e.write(symbol(elf,'rv'),rival+bytes(6*23))
            e.write(0x3bf0,bytes((0x20,fn&255,fn>>8,0x4c,0xf3,0x3b)))
            for key,value in [('P',0),('SP',253),('MPR3',124),('MPR6',108),('PC',0x3bf0)]:
                e.call('register_set',key,value)
            e.run(12)
            # Resolve the rendered rival from cache owners and the hardware
            # SAT, checking its patterns against the baked object as well.
            candidates=[]
            ids=struct.unpack('<48H',e.memory(symbol(elf,'sprite_ids'),96))
            words=struct.unpack('<48H',e.memory(symbol(elf,'sprite_words'),96))
            sat=c.sat(e,0)
            for slot,id in enumerate(ids):
                if not 9<=id<15:continue
                hits=[entry for entry in sat if entry[0] and words[slot]>>5<=entry[2]<(words[slot]>>5)+scene['sprites'][id]['entries']*2]
                if not hits:continue
                pat=struct.unpack_from('<I',blob,scene['sprite_table']+id*16)[0]
                for y,x,p,a in hits:
                    piece=(p-(words[slot]>>5))//2
                    assert read_space(e,'vram0',p*64,128)==blob[pat+piece*128:pat+(piece+1)*128]
                candidates.append(id)
            n=e.memory(symbol(elf,'nvis'),1)[0]
            points=list(struct.iter_unpack('<hhHB',e.memory(symbol(elf,'vis'),n*7)))
            point=next(v for v in points if v[3]==1)
            assert 40<=point[2]<=560 and point[1]==113+10080//point[2],point
            rows=point[1]-113;dots=rows+(rows>>3)+(rows>>5)
            widths=[32,48,64,80,104,128]
            expected=next((i for i,w in enumerate(widths) if w>=dots),5)
            offscreen=point[1]-scene['sprites'][9+expected]['height']>=224
            if offscreen:
                assert not candidates,('Offscreen rival still submitted',distance,candidates)
                sweep.append(dict(distance=distance,depth=point[2],width=widths[expected],sprite_id=9+expected,offscreen=True))
                continue
            if len(candidates)!=1:
                print('Sweep failure',e.call('registers'),
                      'vis',e.memory(symbol(elf,'vis'),42).hex(),
                      'rv',e.memory(symbol(elf,'rv'),23).hex(),
                      'cache',ids,'sat',sat,flush=True)
            assert len(candidates)==1,(distance,'missing or duplicated rival',candidates)
            id=candidates[0];width=scene['sprites'][id]['width']
            # Test expected perspective as well as monotonicity so choosing
            # the largest pose at every depth cannot pass.
            assert id==9+expected,(distance,id,expected,dots)
            if sweep:assert width>=sweep[-1]['width'],('Car shrank while approaching',sweep[-1],distance,width)
            sweep.append(dict(distance=distance,depth=point[2],width=width,sprite_id=id))
            if distance in (540,200,100):e.screenshot(out/f'race-depth-{distance}.png')
            if distance==200:
                # Native 32-dot cells must reproduce the two 16-dot pieces
                # exactly. Restore partner descriptors and compare the actual
                # hardware-rendered image at the identical halted camera.
                wide_path=out/'race-cells-32.png';narrow_path=out/'race-cells-16.png'
                e.screenshot(wide_path);original=[];wide_count=0
                for sprite in range(scene['sprite_count']):
                    _pat,desc,_pal,count,_w,_h=struct.unpack_from('<IIIHBB',blob,scene['sprite_table']+sprite*16)
                    raw=blob[desc:desc+count*6];parts=list(struct.iter_unpack('<hhH',raw));changed=False
                    for part in range(len(parts)-1):
                        x,y,index=parts[part]
                        if not index&0x8000:continue
                        assert parts[part+1][2]&0x4000
                        parts[part]=(x,y,index&0x3fff)
                        parts[part+1]=(x+16,y,(index&0x3fff)+1)
                        changed=True;wide_count+=1
                    if changed:
                        original.append((desc,raw));e.write(desc,b''.join(struct.pack('<hhH',*p) for p in parts),space='acram')
                assert wide_count>0,'Race assets must use native 32-dot cells'
                for key,value in [('P',0),('SP',253),('MPR3',124),('MPR6',108),('PC',0x3bf0)]:e.call('register_set',key,value)
                e.run(12);e.screenshot(narrow_path)
                assert np.array_equal(np.asarray(Image.open(wide_path)),np.asarray(Image.open(narrow_path))),'Native 32-dot race cells changed rendered pixels'
                for desc,raw in original:e.write(desc,raw,space='acram')
                report['wide_cells']=dict(paired_cells=wide_count,pixel_identical=True)

        assert len({s['width'] for s in sweep})==6,sweep
        report['perspective_sweep']=sweep
    (out/'sgx-race-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print('SGX race restoration and perspective passed',report,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));verify(p.parse_args().out)
