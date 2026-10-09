#!/usr/bin/env python3
"""Exercise native actor retirement and column culling at the SGX herd exit."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol

def verify(out):
    names='pce_metrics herd_reserve herd_draw herd_on herd_pending actors camera frame sat sat_count sat_page sprite_exact sprite_occupancy world_update pce_sgx_herd_front_draw_body'.split()
    a={name:symbol(out/'app.elf',name) for name in names}
    with tempfile.TemporaryDirectory(prefix='sgx-herd-exit-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,a['pce_metrics']);e.run(120)
        # Isolate calls to the compiled runtime while hardware IRQs are masked.
        e.write(0x1402,b'\7');e.write(0x0c01,b'\0')
        def call(name,bank):
            p=a[name];e.write(0x3bf0,bytes([0x20,p&255,p>>8,0x4c,0xf3,0x3b]))
            for key,value in [('P',4),('SP',253),('MPR3',bank),('MPR6',108),('PC',0x3bf0)]:
                e.call('register_set',key,value)
            for _ in range(100):
                e.run(1)
                if e.call('registers')['registers']['PC'] in (0x3bf3,0x3bf4,0x3bf5):return
            raise AssertionError(('Native call did not return',name))
        e.write(a['camera'],struct.pack('<H',2200))
        e.write(a['frame'],bytes(2));e.write(a['herd_pending'],b'\0')
        e.write(a['sat_page'],b'\0');e.write(a['sprite_exact'],b'\1')
        call('herd_reserve',128)
        checks=0
        for sx in range(8,-130,-1):
            # The added horse's left edge is actor X - camera + 40.
            actor=struct.pack('<4h13B',2200+sx-40,168,0,0,0,0,4,4,1,11,1,0,1,0,0,0,0)
            e.write(a['actors'],actor+bytes(7*21))
            e.write(a['herd_on'],b'\1')
            call('world_update',115)
            active=e.memory(a['actors']+12,1)[0]
            assert bool(active)==(sx>=-127),('Horse retired while visible',sx,active)
            e.write(a['sat_count'],b'\0');e.write(a['sprite_occupancy'],bytes(240))
            call('pce_sgx_herd_front_draw_body',128)
            count=e.memory(a['sat_count'],1)[0]
            cells=list(struct.iter_unpack('<4H',e.memory(a['sat'],count*8)))
            moved=sx-2
            expected=sorted((168-48+64+part*64,moved+32*col+32)
                            for col in range(4) if active and -32<moved+32*col<256
                            for part in range(2))
            assert sorted((y,x) for y,x,p,attr in cells)==expected,('Visible columns culled early',sx,cells,expected)
            if sx==-125:
                assert count==2 and all(x==1 for y,x,p,attr in cells),'Last column must survive at X=-127'
            call('herd_draw',111)
            assert bool(e.memory(a['herd_on'],1)[0])==bool(active),('Convoy released before added horse exited',sx)
            checks+=1
        # Normalize world positions to the same screen positions on each VDC.
        # Sweep two horses across both edges, including every column boundary.
        parity_checks=0
        for sx in range(-130,257):
            outputs=[]
            for stagger,name,bank in ((0,'herd_draw',111),(112,'pce_sgx_herd_front_draw_body',128)):
                horses=[2200+sx+72-stagger+k*224 for k in range(2)]
                actors=b''.join(struct.pack('<4h13B',x,168,0,0,0,0,4,4,1,11,1,0,1,0,0,0,0) for x in horses)
                e.write(a['actors'],actors+bytes(6*21));e.write(a['herd_on'],b'\1')
                e.write(a['sat_count'],b'\0');e.write(a['sprite_occupancy'],bytes(240))
                call(name,bank)
                count=e.memory(a['sat_count'],1)[0]
                cells=list(struct.iter_unpack('<4H',e.memory(a['sat'],count*8)))
                outputs.append(sorted((y,x,attr) for y,x,p,attr in cells))
            expected=sorted((168-48+64+part*64,sx+k*224+col*32+32,0x318d if part==0 else 0x18d)
                            for k in range(2) for col in range(4)
                            if -32<sx+k*224+col*32<256 for part in range(2))
            assert outputs[0]==outputs[1]==expected,('VDC horse clipping differs',sx,outputs,expected)
            parity_checks+=1
    report=dict(exit_positions=checks,vdc_parity_positions=parity_checks,horses_per_vdc=2,last_visible_x=-127,fully_culled_x=-128,passed=True)
    (out/'sgx-herd-exit-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'))
    verify(p.parse_args().out.resolve())
