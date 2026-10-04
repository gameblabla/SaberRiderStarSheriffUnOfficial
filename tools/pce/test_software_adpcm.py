#!/usr/bin/env python3
"""Execute both native decoders against Build 14, including counter/loop edges."""
import argparse,json,struct,tempfile
from pathlib import Path
from adpcm2 import tables,advance,encode
from emulator import Emulator,boot,symbol

def verify(out):
    elf=out/'app.elf';state=symbol(elf,'pce_pcm_voices');step=symbol(elf,'pce_pcm_step')
    t=tables();checked=0;start=0xc0f0
    with tempfile.TemporaryDirectory(prefix='software-adpcm-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,symbol(elf,'pce_metrics'))
        e.write(0x0c01,b'\0');e.write(0x1402,b'\x07')
        for key,val in [('P',4),('MPR3',117),('MPR6',125)]:e.call('register_set',key,val)
        streams=[bytes([v])*17 for v in (0,255,0x55,0xaa,0xe4)]+[encode([int(24000*((i%11)-5)/5) for i in range(67)],t)]
        for ch in range(2):
            cases=[(packed,count,predictor,index,0) for packed in streams
                   for count in (1,2,3,7,67) for predictor,index in ((32768,0),(0,255),(65535,255))]
            # Borrow across the low counter byte; packed reads cross a page.
            cases += [(bytes([0xe4])*65,count,32768,0,0) for count in (255,256,257)]
            cases += [(bytes([0xe4])*2,7,32768,0,1)]
            for packed,count,predictor,index,loop in cases:
                e.write(start,packed)
                e.write(state+16*ch,struct.pack('<HHH6BHH',count,start,predictor,index,0,0,125,loop,ch,start,count))
                for i in range(count):
                    code=bytes([0xa2,ch*16,0x20,step&255,step>>8,0x4c,5,0x3b])
                    e.write(0x3b00,code);e.call('register_set','SP',253);e.call('register_set','PC',0x3b00);e.run(1)
                    predictor,index=advance(predictor,index,(packed[i>>2]>>((i&3)*2))&3,t)
                    actual=struct.unpack('<HHH6BHH',e.memory(state+16*ch,16))
                    if loop and i==count-1:
                        assert actual[:4]==(count,start,32768,0) and actual[5]==0,actual
                    else:
                        assert actual[:4]==(count-i-1,start+(i//4)+1,predictor,index),(ch,i,actual,predictor,index)
                        assert actual[5]==(3-i)%4,actual
                    assert e.call('registers')['registers']['MPR6']==125,'Fetch must restore MPR6'
                    checked+=1
    report=dict(samples_compared=checked,channels=2,counter_borrow=True,page_crossing=True,partial_byte_loop=True,passed=True)
    (out/'software-adpcm-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
