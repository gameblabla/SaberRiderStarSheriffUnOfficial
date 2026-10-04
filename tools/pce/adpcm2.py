#!/usr/bin/env python3
"""Compressor for the supplied adpcm_build_14_2bit.pce's actual table decoder.

Codes are low pair first: +small, +large, -small, -large. Predictor is unsigned
16-bit, starts at 0x8000 and saturates; the ring-buffer output is its high byte.
The adaptation tables come from ROM bank 3, as read by native code C6A1-C8BD.
A JSON sidecar carries the sample count expected by the demo's length table.
The game uses these tables/streams in its timer-driven PSG DDA decoder.
"""
import argparse
import array
import hashlib
import json
from pathlib import Path
import subprocess
import wave

ROM=Path(__file__).resolve().parents[3]/'PCE/Sound/adpcm_build_14_2bit.pce'

def tables(rom=ROM):
    data=Path(rom).read_bytes()
    if len(data)!=1048576 or data[0x262e:0x2631]!=bytes.fromhex('4c45c6'):
        raise ValueError('Expected the supplied build 14 2-bit ROM decoder')
    a=data[0x6000:0x6600]
    small=[a[i]|a[512+i]<<8 for i in range(256)]
    large=[a[256+i]|a[768+i]<<8 for i in range(256)]
    return small,large,a[1024:1280],a[1280:1536]

def advance(predictor,index,code,t):
    small,large,next_small,next_large=t
    magnitude=(large if code&1 else small)[index]
    predictor=max(0,min(65535,predictor+(-magnitude if code&2 else magnitude)))
    index=(next_large if code&1 else next_small)[index]
    return predictor,index

def decode(data,count=None,t=None):
    if t is None:t=tables()
    if count is None:count=len(data)*4
    if not 0<=count<=len(data)*4:raise ValueError('Invalid sample count')
    predictor,index=32768,0;out=[]
    for i in range(count):
        code=data[i>>2]>>((i&3)*2)&3
        predictor,index=advance(predictor,index,code,t)
        out.append((predictor>>8)-128)
    return out

def encode(pcm,t=None):
    if t is None:t=tables()
    predictor,index=32768,0;codes=[]
    for sample in pcm:
        target=max(0,min(65535,int(sample)+32768))
        candidates=[advance(predictor,index,c,t) for c in range(4)]
        code=min(range(4),key=lambda c:abs(candidates[c][0]-target))
        predictor,index=candidates[code];codes.append(code)
    return bytes(sum(c<<((i&3)*2) for i,c in enumerate(codes[start:start+4])) for start in range(0,len(codes),4))

def compress(src,dst,rate=15734,rom=ROM):
    result=subprocess.run(['ffmpeg','-v','error','-i',str(src),'-ac','1','-ar',str(rate),
        '-af',f'lowpass=f={min(7000,rate*0.44):g}','-f','s16le','-'],check=True,capture_output=True)
    pcm=array.array('h');pcm.frombytes(result.stdout)
    t=tables(rom);data=encode(pcm,t);dst=Path(dst);dst.write_bytes(data)
    reconstructed=decode(data,len(pcm),t)
    with wave.open(str(dst.with_suffix('.decoded.wav')),'wb') as w:
        w.setnchannels(1);w.setsampwidth(2);w.setframerate(rate)
        w.writeframes(array.array('h',(v*256 for v in reconstructed)).tobytes())
    report=dict(format='build14-2bit-low-pair-first',samples=len(pcm),bytes=len(data),rate=rate,
        predictor=32768,step_index=0,rom_sha256=hashlib.sha256(Path(rom).read_bytes()).hexdigest())
    dst.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--rate',type=int,default=15734);p.add_argument('--rom',type=Path,default=ROM)
    a=p.parse_args();print(json.dumps(compress(a.input,a.output,a.rate,a.rom)))
