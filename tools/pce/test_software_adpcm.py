#!/usr/bin/env python3
"""Check exact Build 14 DAC assets and native playback, bank/count/loop edges."""
import argparse,json,struct,tempfile
from pathlib import Path
from adpcm2 import decode,tables
from emulator import Emulator,boot,symbol

def verify(out):
    elf=out/'app.elf';state=symbol(elf,'pce_pcm_voices');step=symbol(elf,'pce_pcm_step')
    checked=0;asset_samples=0
    for row in json.loads((out/'audio.json').read_text())['dda']:
        event=row['event'];packed=(out/'work'/f'{event}.adpcm2').read_bytes()
        expected=bytes((v+128)>>3 for v in decode(packed,row['samples'],tables()))
        assert (out/'work'/f'{event}.dda').read_bytes()==expected,event
        asset_samples+=len(expected)
    with tempfile.TemporaryDirectory(prefix='software-adpcm-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,symbol(elf,'pce_metrics'))
        e.write(0x0c01,b'\0');e.write(0x1402,b'\x07')
        for key,val in [('P',4),('MPR3',117),('MPR6',125)]:e.call('register_set',key,val)
        # Real generated arrays must survive normal boot and CD scratch loading.
        # Shot is pure PSG (no DDA file); resident holds the power sample only.
        resident=symbol(elf,'pce_pcm_resident')
        resident_data=(out/'work/power.dda').read_bytes()
        assert e.memory(resident,len(resident_data))==resident_data
        stream=(out/'work/impact.dda').read_bytes()+(out/'work/gallop.dda').read_bytes()+(out/'work/tick.dda').read_bytes()
        for i in range(3):
            e.call('register_set','MPR6',125+i)
            assert e.memory(0xc000,min(8192,len(stream)-8192*i))==stream[8192*i:8192*(i+1)]
        e.call('register_set','MPR6',125)
        for ch in range(2):
            for count,loop in [(1,0),(2,0),(3,0),(7,0),(255,0),(256,0),(257,0),(7,1)]:
                start=0xdff0;data=bytes(i%32 for i in range(count))
                e.write(start,data[:16]);e.call('register_set','MPR6',126)
                e.write(0xc000,data[16:]);e.call('register_set','MPR6',125)
                e.write(state+16*ch,struct.pack('<HHH6BHH',count,start,0,125,0,0,125,loop,ch,start,count))
                e.write(symbol(elf,'pce_pcm_active'),bytes([1<<ch]))
                for i in range(count*(2 if loop else 1)):
                    code=bytes([0xa2,ch*16,0x20,step&255,step>>8,0x4c,0xf5,0x3b])
                    e.write(0x3bf0,code)
                    for key,val in [('SP',253),('PC',0x3bf0)]:e.call('register_set',key,val)
                    e.run(1);actual=struct.unpack('<HHH6BHH',e.memory(state+16*ch,16))
                    n=i%count
                    assert actual[2]==data[n],(ch,i,actual)
                    if loop and n==count-1:
                        assert actual[:2]==(count,start) and actual[6]==125,actual
                    else:
                        read=start+n+1;bank=125
                        if read>=0xe000:read-=8192;bank+=1
                        assert actual[:2]==(count-n-1,read) and actual[6]==bank,actual
                    assert e.call('registers')['registers']['MPR6']==125,'Restore caller MPR6'
                    checked+=1
                if not loop:assert e.memory(symbol(elf,'pce_pcm_active'),1)==b'\0'
    report=dict(asset_samples_compared=asset_samples,native_samples_compared=checked,channels=2,
                counter_borrow=True,bank_crossing=True,loop_bank_reset=True,passed=True)
    (out/'software-adpcm-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));verify(p.parse_args().out.resolve())
