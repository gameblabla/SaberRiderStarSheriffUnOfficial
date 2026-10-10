#!/usr/bin/env python3
"""Compare the native lookup road with the retained knot renderer at fixed poses."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
from test_road import tables

def verify(out):
    out=out.resolve();elf=out/'app.elf';c=Campaign(out,sgx=True)
    manifest=json.loads((out/'manifest.json').read_text());scene=manifest['scenes'][1];blob=(out/'s2.bin').read_bytes()
    offset=scene['records']['road_curves']['offset'];trackoff=scene['records']['track']['offset']
    track=list(struct.iter_unpack('<HHB',blob[trackoff:trackoff+1280]))
    import re
    sin=list(map(int,re.findall(r'-?\d+',(Path('src/platform/pce/race_math.h').read_text().split('SIN[256]')[1].split('={',1)[1].split('};',1)[0]))))
    def wrap(a,b):return ((a-b+4096)&8191)-4096
    def sine14(h):
        i=(h>>8)&255;return sin[i]*128+((sin[(i+1)&255]-sin[i])*(h&255)>>1)
    report=[]
    with tempfile.TemporaryDirectory(prefix='road-curves-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);c.seed(e,'stage',1,1);c.field(e,'state',2);c.advance(e,2);c.dialogs(e)
        fn=symbol(elf,'road_draw');stub=bytes((0x20,fn&255,fn>>8,0x4c,0xf3,0x3b))
        def draw(reference):
            c.seed(e,'road_reference',reference,1);e.write(0x3bf0,stub)
            for key,value in [('P',0),('SP',253),('MPR3',109),('PC',0x3bf0)]:e.call('register_set',key,value)
            e.run(3);return tables(e,out)[0]
        for sample in range(0,4096,128):
            camera_x,camera_y,heading=struct.unpack_from('<3H',blob,offset+sample*128+96)
            coarse_c=(sine14(heading+16384)+64)>>7;coarse_s=(sine14(heading)+64)>>7
            px=camera_x+(coarse_c*92>>7);py=camera_y+(coarse_s*92>>7);idx=sample>>4
            bi=min(((idx+d)&255 for d in range(-2,4)),key=lambda j:abs(wrap(px,track[j][0]))+abs(wrap(py,track[j][1])))
            for lat,yaw in ((0,0),(-120,0),(120,0),(0,-256),(0,256)):
                h=(heading+yaw)&65535;qcos=sine14(h+16384);qsin=sine14(h)
                cc,ss=(qcos+64)>>7,(qsin+64)>>7
                # Shift the camera right, including its 92-unit backing shift
                # when its heading lags the circuit tangent.
                x=camera_x-((coarse_s*lat)>>7)+((coarse_c-cc)*92>>7)
                y=camera_y+((coarse_c*lat)>>7)+((coarse_s-ss)*92>>7)
                for name,val,size in [('ps',sample<<4,2),('road_idx',bi,1),('rphase',1,1),('cam_hd',h,2),('cam_c',cc,1),('cam_s',ss,1),('cam_cl',qcos-cc*128,1),('cam_sl',qsin-ss*128,1)]:c.seed(e,name,val,size)
                c.seed(e,'pce_control.0',x&8191);c.seed(e,'pce_control.1',y&8191)
                old,new=draw(1),draw(0)
                def signed(v):return (v+32768)%65536-32768
                errors=[abs(signed(old[m])-signed(new[m])) for m in range(16,112,2)]
                report.append(dict(sample=sample,lat=lat,yaw=yaw,max_error=max(errors),near_error=max(errors[24:])))
                if sample in (0,1024,2048,3072) and not lat and not yaw:
                    e.screenshot(out/f'curve-{sample}.png')
    (out/'road-curves-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Lookup road comparison:',len(report),'poses; max',max(r['max_error'] for r in report),'dots; near',max(r['near_error'] for r in report),'dots',flush=True)
    zero=[r for r in report if not r['lat'] and not r['yaw']]
    assert max(r['max_error'] for r in zero)<=1,('Baked reference geometry differs',zero)
    assert max(r['near_error'] for r in report)<=8,('Nearby road camera correction diverged',report)
    return report
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));verify(p.parse_args().out)
