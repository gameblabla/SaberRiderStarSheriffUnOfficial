#!/usr/bin/env python3
"""Check the host codec against the supplied ROM's native HuC6280 decoder.

Seeds ROM registers and RAM to call C62E directly. This checks bit order,
clamping, predictor and index semantics; it does not measure the demo's CPU use.
"""
import argparse
import json
from pathlib import Path
import struct
import tempfile
import numpy as np
from adpcm2 import ROM,tables,encode,decode,advance
from emulator import Emulator

def verify(out):
    t=tables();cases=0;checked=0
    with tempfile.TemporaryDirectory(prefix='codec-',dir=out) as base,Emulator(ROM,base) as e:
        e.run(120)
        for count in (1,2,3,4,7,15,63,64):
            streams=[bytes([0]*16),bytes([255]*16),bytes([0x55]*16),bytes([0xaa]*16),bytes(range(16)),
                     encode((np.sin(np.arange(64)*0.5)*24000).astype(int),t)]
            for packed in streams:
                # Stop normal ROM IRQs; install a RAM return-loop for the direct call.
                e.write(0x0c01,b'\0');e.write(0x1402,b'\x07')
                for name,value in [('P',4),('SP',253),('MPR0',255),('MPR1',248),('MPR5',3),('MPR6',1)]:
                    e.call('register_set',name,value)
                e.write(0x2300,packed);e.write(0x2400,bytes.fromhex('4c0024'))
                e.write(0x21fe,bytes.fromhex('ff23'))
                e.write(0x2075,bytes([0,0,0x23,0,count,0,0,0,128,0]))
                e.write(0x2c00,bytes([0x55])*128)
                e.call('register_set','PC',0xc62e);e.run(1)
                actual=list(e.memory(0x2c00,count));expected=[v+128 for v in decode(packed,count,t)]
                assert actual==expected,(count,packed.hex(),actual,expected)
                assert e.memory(0x2075,1)[0]==count
                predictor,index=32768,0
                for i in range(count):predictor,index=advance(predictor,index,packed[i>>2]>>((i&3)*2)&3,t)
                assert int.from_bytes(e.memory(0x207c,2),'little')==predictor
                assert e.memory(0x207e,1)[0]==index
                cases+=1;checked+=count
    report=dict(native_decoder_cases=cases,samples_compared=checked,passed=True,
                hardware_playback='not installed in game',cpu_budget='not measured')
    (out/'adpcm2-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));a=p.parse_args();verify(a.out.resolve())
