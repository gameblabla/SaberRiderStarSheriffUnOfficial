#!/usr/bin/env python3
"""The race's classic road: screenshots down the first straight and through the first bend, the road's refresh rate, the cycle
profile of a frame, and checks that the road follows the circuit (the road's centre on the screen moves with the camera)."""
import argparse
import json
from pathlib import Path
import struct
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent))
from emulator import Emulator,boot,symbol
from test_campaign import Campaign
from test_port import FIELDS
KEY_1,KEY_2,KEY_UP,KEY_RIGHT,KEY_DOWN,KEY_LEFT=1,2,16,32,64,128

def raw_metrics(e,c):
    return dict(zip(FIELDS,struct.unpack('<4s4B8H4B4H',e.memory(c.address,36))))

def tables(e,out,page=None):
    """The road's BXR (low, high) and picture copy of each scanline, of the buffer on display."""
    elf=out/'app.elf'
    columns=symbol(elf,'columns')
    shown=e.memory(symbol(elf,'pce_floor_page'),1)[0]
    raw=e.memory(columns,768)
    off=128*(shown if page is None else page)
    lo,hi,sel=raw[off:off+112],raw[256+off:256+off+112],raw[512+off:512+off+112]
    return [lo[i]|hi[i]<<8 for i in range(112)],list(sel)

def autopilot(e,out,state):
    """Steer for the circuit point 4 samples ahead (reads the car's position and heading from the console RAM)."""
    import math
    elf=out/'app.elf'
    px,py,hd=struct.unpack('<3H',e.memory(symbol(elf,'px'),2)+e.memory(symbol(elf,'py'),2)+e.memory(symbol(elf,'hd'),2))
    idx=e.memory(symbol(elf,'road_idx'),1)[0]
    track=symbol(elf,'track')
    raw=e.memory(track+((idx+4)&255)*5,4)
    tx,ty=struct.unpack('<2H',raw)
    dx=((tx-px+4096)&8191)-4096;dy=((ty-py+4096)&8191)-4096
    want=math.atan2(dy,dx)/(2*math.pi)*65536
    diff=((want-hd+32768)%65536)-32768
    keys=KEY_UP
    if diff>1200:keys|=KEY_RIGHT
    elif diff<-1200:keys|=KEY_LEFT
    return keys

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));p.add_argument('--tag',default='road')
    p.add_argument('--require-fps',type=float,default=0);p.add_argument('--sgx',action='store_true');p.add_argument('--seconds',type=int,default=12);p.add_argument('--profile',action='store_true')
    args=p.parse_args();out=args.out.resolve();c=Campaign(out,sgx=args.sgx);report={}
    with tempfile.TemporaryDirectory(dir=out) as b,Emulator(out/'saber_rider.cue',b,sgx=args.sgx) as e:
        boot(e,c.address)
        # Retail builds have no pause-menu stage selector. Seed the next-stage
        # number and exercise the native clear/load path, as the SGX fixtures.
        c.seed(e,'stage',1,1);c.field(e,'state',2);c.advance(e,2);c.dialogs(e);e.run(240)
        e.screenshot(out/f'{args.tag}-grid.png')
        bxr,sel=tables(e,out);report['grid_bxr']=bxr[::8]
        e.input(KEY_UP|KEY_2);m0=raw_metrics(e,c)
        presented=symbol(out/'app.elf','pce_presented');p0=int.from_bytes(e.memory(presented,2),'little')
        for k in range(args.seconds):
            for _ in range(12):
                e.run(5);e.input(autopilot(e,out,None)|(KEY_2 if k%4<2 else 0))
            e.screenshot(out/f'{args.tag}-{k:02d}.png')
            bxr,sel=tables(e,out)
            print(k,'ovf',raw_metrics(e,c)['essential_overflow'],'sat',raw_metrics(e,c)['sat_count'],'bxr',bxr[::12],'sel',''.join(map(str,sel[::2])),flush=True)
        m1=raw_metrics(e,c)
        report['commits_per_second']=((m1['floor_commits']-m0['floor_commits'])&65535)/args.seconds
        p1=int.from_bytes(e.memory(presented,2),'little')
        report['presentations_per_second']=((p1-p0)&65535)/args.seconds
        report['essential_overflow']=m1['essential_overflow'];report['video_frames_per_second']=((m1['frames']-m0['frames'])&65535)/args.seconds
        print(report,flush=True)
        if args.profile:
            e.call('prof_start');e.run(300);path=out/f'{args.tag}-cycles.txt';e.call('prof_dump',str(path))
    (out/f'{args.tag}-report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['commits_per_second']>=args.require_fps,('Road cadence below required rate',report,args.require_fps)
    assert report['presentations_per_second']>=args.require_fps-0.2,('SAT presentation cadence below required rate',report,args.require_fps)

if __name__=='__main__':main()
