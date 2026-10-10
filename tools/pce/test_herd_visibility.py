#!/usr/bin/env python3
"""Sweep complete native platform draws; catch partial horses under real SAT limits.

Positions and firing pose are seeded; the compiled draw code, cache, HUD,
streamer and admission run normally. This is an isolated rendering regression,
separate from live trigger/audio/60 Hz checks.
"""
import argparse,struct,json,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
def verify(out):
    elf=out/'app.elf'
    names='pce_metrics herd_on herd_pending herd_reserve actors player camera frame sat sat_count sat_page sprite_exact safe_timer hero facing fire_timer play_draw pce_scroll_y pce_sat_pending pce_display_on vce_q_head vce_q_tail vce_hold'.split();a={n:symbol(elf,n) for n in names}
    with tempfile.TemporaryDirectory(prefix='herd-visibility-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,a['pce_metrics']);e.run(120);e.write(0x1402,b'\7');e.write(0x0c01,b'\0')
        # With IRQs masked, keep the VCE off its deferred VBlank queue; the
        # direct draw fixture writes palettes synchronously instead.
        for name in ('pce_display_on','vce_q_head','vce_q_tail','vce_hold'):e.write(a[name],b'\0')
        def call(n,bank):
            p=a[n];e.write(0x3bf0,bytes([0x20,p&255,p>>8,0x4c,0xf3,0x3b]))
            for k,v in [('P',4),('SP',253),('MPR3',bank),('MPR6',108),('PC',0x3bf0)]:e.call('register_set',k,v)
            for _ in range(100):
                e.run(1)
                if e.call('registers')['registers']['PC'] in (0x3bf3,0x3bf4,0x3bf5):break
            else:raise AssertionError((n,e.call('registers')))
        call('herd_reserve',111);e.write(a['camera'],struct.pack('<H',2200));e.write(a['safe_timer'],b'\0');e.write(a['sprite_exact'],b'\1')
        peak=0
        for h in range(4):
            hud_y=None
            e.write(symbol(elf,'pce_control.3'),bytes([h]))
            e.write(a['hero'],bytes([h]));e.write(a['fire_timer'],b'\10')
            for phase in range(256):
                horses=[(2200+phase+i*224-224,168) for i in range(3)]
                actors=b''.join(struct.pack('<4h13B',x,y,0,0,0,0,4,4,1,11,1,0,1,0,0,0,0) for x,y in horses)
                e.write(a['actors'],actors+bytes(168-len(actors)));e.write(a['player'],struct.pack('<4h4B',2320,160,0,0,0,0,4,4));e.write(a['frame'],struct.pack('<H',phase))
                # The IRQ is masked, so acknowledge each synthetic SAT
                # publication before calling the next isolated draw.
                e.write(a['pce_sat_pending'],b'\0')
                call('play_draw',123)
                count=e.memory(a['sat_count'],1)[0];page=e.memory(a['sat_page'],1)[0];sat=list(struct.iter_unpack('<4H',e.memory(a['sat']+page*512,count*8)))
                shake=(phase*13^(phase>>2))&3
                assert int.from_bytes(e.memory(a['pce_scroll_y'],2),'little')==shake
                hud=tuple((y,x) for y,x,p,attr in sat if (attr&15)<3)
                if hud_y is None:hud_y=hud
                assert hud==hud_y,'The HUD must stay steady during world shake'
                actual=sum((attr&15)==15 for y,x,p,attr in sat)
                expected_xy=sorted((ay-48+64+part*64-shake,ax-2200-72+col*32+32)
           for ax,ay in horses for col in range(4) if -32<ax-2200-72+col*32<256 for part in range(2))
                actual_xy=sorted((y,x) for y,x,p,attr in sat if (attr&15)==15)
                assert actual_xy==expected_xy,(h,phase,'complete horse and world shake',actual_xy,expected_xy)
                expected=sum(2 for ax,ay in horses for col in range(4) if -32<ax-2200-72+col*32<256)
                assert actual==expected,(h,phase,actual,expected,sat,e.memory(a['actors'],168).hex())
                lines=[0]*224
                for y,x,p,attr in sat:
                    y=(y&1023)-64;height=(16,32,64,64)[attr>>12&3]
                    for l in range(max(0,y),min(224,y+height)):lines[l]+=2 if attr&256 else 1
                assert max(lines)<=16,(h,phase,max(lines))
                peak=max(peak,max(lines))
        report=dict(draws=1024,heroes=4,horizontal_phases=256,max_scanline_units=peak,shake_offsets=[0,1,2,3],complete_horses=True,fixed_hud=True,passed=True)
        (out/'herd-visibility-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)

if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("--out",type=Path,default=Path("build/pce"));verify(p.parse_args().out.resolve())
