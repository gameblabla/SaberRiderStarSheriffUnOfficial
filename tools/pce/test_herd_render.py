#!/usr/bin/env python3
"""Native herd SAT/scanline equivalence and exact double-buffer contents."""
import argparse,json,random,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol

def reference(horses,camera,code,count,occupancy,exact):
    entries=[];occ=list(occupancy)
    for ax,ay in horses:
        sx=ax-camera-72;sy=ay-48
        for col in range(4):
            x=sx+32*col
            if x<=-32 or x>=256:continue
            for part in range(2):
                y=sy+64*part;h=16 if part else 64
                lo=max(y,0);hi=min(y+h,224)
                if count>=64 or hi<=lo:continue
                lines=range(lo,hi) if exact else range(lo>>3,((hi-1)>>3)+1)
                if any(occ[line]>14 for line in lines):continue
                for line in lines:occ[line]+=2
                entries.append(struct.pack('<4H',(y+64)&65535,x+32,code+(32+2*col if part else 8*col)*2,0x18f if part else 0x318f))
                count+=1
    return b''.join(entries),count,bytes(occ)

def verify(out):
    elf=out/'app.elf';rng=random.Random(6280);checks=0
    names='herd_draw actors camera frame sat sat_count sat_page sprite_occupancy sprite_exact cur shown prefetch prefetched play_scene'.split()
    addr={name:symbol(elf,name) for name in names}
    with tempfile.TemporaryDirectory(prefix='herd-render-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,symbol(elf,'pce_metrics'));e.run(120)
        # Inject isolated calls into resident console RAM. Mask all hardware
        # IRQs; C helpers may CLI, but none can interrupt these test calls.
        e.write(0x1402,b'\x07');e.write(0x0c01,b'\0')
        def call(name):
            a=symbol(elf,name)
            e.write(0x3b00,bytes([0x20,a&255,a>>8,0x4c,3,0x3b]))
            bank=110 if name=='presentation_frame' else 111
            for key,val in [('P',4),('SP',253),('MPR3',bank),('MPR6',108),('PC',0x3b00)]:e.call('register_set',key,val)
            e.run(2)
        def seed(name,data):e.write(addr[name],data)
        call('herd_reserve')
        for exact in (0,1):
            for case in range(80):
                camera=2000;count=rng.choice([0,20,50,60,63,64]);page=case&1
                horses=[(camera+rng.choice([-200,-60,40,72,180,290,328,500]),rng.choice([-32,32,48,55,160,230,270])) for _ in range(rng.randrange(1,6))]
                actors=b''.join(struct.pack('<4h13B',x,y,0,0,0,0,4,4,1,11,1,0,1,0,0,0,0) for x,y in horses)
                seed('actors',actors+bytes(21*(8-len(horses))))
                seed('camera',struct.pack('<H',camera));seed('frame',b'\0\0')
                seed('sat_count',bytes([count]));seed('sat_page',bytes([page]));seed('sprite_exact',bytes([exact]))
                occ=bytes(rng.randrange(17) for _ in range(240)) if case%3==0 else bytes([rng.randrange(17)])*240
                seed('sprite_occupancy',occ)
                call('herd_draw')
                cur=e.memory(addr['cur'],1)[0];code=(0x4800+(28+cur*10)*256)>>5
                entries,n,expected=reference(horses,camera,code,count,occ,exact)
                assert e.memory(addr['sat_count'],1)[0]==n,(exact,case,horses)
                actual=e.memory(addr['sat']+page*512+count*8,len(entries))
                assert actual==entries,(exact,case,horses,count,actual.hex(),entries.hex())
                assert e.memory(addr['sprite_occupancy'],240)==expected,(exact,case,'occupancy')
                checks+=1
            # Clearing must touch precisely the active representation.
            seed('sprite_occupancy',b'\xaa'*240);call('sprite_lines_clear')
            n=240 if exact else 32
            assert e.memory(addr['sprite_occupancy'],240)==bytes(n)+b'\xaa'*(240-n)
        # All five original 5120-byte frames, including wrap and skipped ticks.
        scene=int.from_bytes(e.memory(addr['play_scene'],2),'little')
        horse=int.from_bytes(e.memory(scene+48,4),'little')
        source=(out/'s1.bin').read_bytes()[horse+32:horse+32+5*5120]
        assert len(source)==25600
        seed('actors',struct.pack('<4h13B',2072,160,0,0,0,0,4,4,1,11,1,0,1,0,0,0,0)+bytes(7*21))
        seed('shown',b'\xff');seed('prefetch',b'\xff');seed('prefetched',b'\0\0')
        seed('sat_page',b'\0');seed('sprite_exact',b'\1')
        for tick in list(range(24))+[26,28,33,40,41,42,43,44]:
            seed('frame',struct.pack('<H',tick));seed('sat_count',b'\0');seed('sprite_occupancy',bytes(240))
            oldcur=e.memory(addr['cur'],1)[0]
            old=bytes.fromhex(e.call('asread','vram0',(0x4800+(28+oldcur*10)*256)*2,5120)['hex'])
            call('herd_draw')
            cur=e.memory(addr['cur'],1)[0];want=(tick>>2)%5
            actual=bytes.fromhex(e.call('asread','vram0',(0x4800+(28+cur*10)*256)*2,5120)['hex'])
            assert actual==source[want*5120:(want+1)*5120],('frame bytes',tick)
            if cur!=oldcur and tick%4==0:
                assert bytes.fromhex(e.call('asread','vram0',(0x4800+(28+oldcur*10)*256)*2,5120)['hex'])==old,('display buffer',tick)
        # Compare retained HUD replay with a fresh native build for every
        # sheriff/HP combination and both occupancy representations.
        try:hero_address=symbol(elf,'pce_control')+5
        except ValueError:hero_address=symbol(elf,'pce_control.3') # LLVM SROA: field 3 is hero
        metrics=symbol(elf,'pce_metrics')
        campaign=symbol(elf,'pce_campaign');hud_ready=symbol(elf,'hud_ready')
        hud_cases=0
        for exact in (0,1):
            seed('sprite_exact',bytes([exact]));e.write(symbol(elf,'fg_entered'),b'\1')
            for hero in range(4):
                for hp in range(4):
                    e.write(hero_address,bytes([hero]));e.write(metrics+34,struct.pack('<H',hp))
                    e.write(campaign+1,bytes([3+hero,hp]))
                    for _ in range(3):call('video_sat_begin')
                    e.write(hud_ready,b'\0');call('presentation_frame')
                    n=e.memory(addr['sat_count'],1)[0]
                    assert n>=14,('HUD admission',hero,hp,n)
                    expected_sat=e.memory(addr['sat'],n*8)
                    expected_lines=e.memory(addr['sprite_occupancy'],240)
                    call('video_sat_begin');call('presentation_frame')
                    assert e.memory(addr['sat_count'],1)[0]==n
                    assert e.memory(addr['sat'],n*8)==expected_sat,('HUD SAT',hero,hp)
                    assert e.memory(addr['sprite_occupancy'],240)==expected_lines,('HUD lines',hero,hp)
                    hud_cases+=1
    report=dict(sat_and_occupancy_cases=checks,hud_cases=hud_cases,animation_frames=5,stream_ticks=32,passed=True)
    (out/'herd-render-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
