#!/usr/bin/env python3
"""Check pause resource lifetime each video frame and independent boss/hit PSG."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def read(e,space,address,size):
    return bytes.fromhex(e.call('asread',space,address,size)['hex'])

def verify(out,sgx):
    out=out.resolve();c=Campaign(out,sgx=sgx);elf=out/'app.elf';report={}
    first=(0x4200+0x540)>>5;patterns=(0x4200+0x540)*2
    with tempfile.TemporaryDirectory(prefix='pause-audio-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=sgx) as e:
        boot(e,c.address);e.run(120)
        # Hold gameplay through the normal RUN input before poisoning the HUD
        # palette. This models a palette not loaded yet / a held screen flash.
        c.press(e,8);e.run(30)
        saved=e.memory(symbol(elf,'buffer')+1024,928)
        c.press(e,8);e.run(30)
        assert read(e,'vram0',patterns,384)==saved[512:896]
        # Temporarily blacken all palettes and hold palette writes. PAUSE
        # must get its colors from an unused slot, even when the HUD is black.
        e.call('aswrite','pram',0,bytes(1024).hex())
        e.write(symbol(elf,'vce_hold'),b'\1')
        c.press(e,8);e.run(30)
        rows=list(struct.iter_unpack('<4H',read(e,'sat0',0,512)))
        assert [r[2] for r in rows[:3]]==[first+2*i for i in range(3)]
        palette=rows[0][3]&15
        assert all((r[3]&15)==palette for r in rows[:3])
        assert not any(y&1023 and (a&15)==palette for y,x,p,a in rows[3:])
        colors=read(e,'pram',(16+palette)*32,32)
        assert struct.unpack_from('<HH',colors,2)==(511,0),colors.hex()
        label=read(e,'vram0',patterns,384)
        e.screenshot(out/'pause-explicit-colors.png')
        saved=e.memory(symbol(elf,'buffer')+1024,928);font=saved[512:896]
        e.input(8);observed=0
        for _ in range(120):
            e.run(1)
            rows=list(struct.iter_unpack('<4H',read(e,'sat0',0,512)))
            data=read(e,'vram0',patterns,384)
            if any(y&1023 and first<=p<first+6 for y,x,p,a in rows):
                assert data==label,'Live pause sprites referenced restored font patterns'
                observed+=1
            if (data==font and not e.memory(symbol(elf,'paused'),1)[0] and
                read(e,'pram',(16+palette)*32,32)==saved[896:928]):break
        else:raise AssertionError('Unpause did not restore the borrowed font and palette')
        assert read(e,'pram',(16+palette)*32,32)==saved[896:928]
        e.input(0);e.write(symbol(elf,'vce_hold'),b'\0')
        report['pause']=dict(palette=palette,observed_label_frames=observed,font_restored=True,colors_restored=True)
        # Isolated compiled audio calls: no gameplay can replace these cues.
        def call(name,tone=0):
            address=symbol(elf,name)
            e.write(0x3bf0,bytes([0xa9,tone,0x20,address&255,address>>8,0x4c,0xf5,0x3b]))
            for key,value in [('P',0),('SP',253),('MPR3',105),('MPR6',108),('PC',0x3bf0)]:e.call('register_set',key,value)
            e.run(1)
        live=symbol(elf,'psg_live')
        for tone in (13,14,15):
            call('audio_stop');call('audio_effect',tone)
            assert e.memory(live,1)==b'\2'
            call('audio_effect',22)
            assert e.memory(live,1)==b'\3','Hit zap replaced the boss gun voice'
            call('audio_effect',tone)
            assert e.memory(live,1)==b'\3','Boss gun replaced the hit voice'
        report['boss_gun_and_hit_voices_independent']=True
    (out/'pause-audio-verification.json').write_text(json.dumps(report,indent=2)+'\n');print(report)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));p.add_argument('--sgx',action='store_true')
    a=p.parse_args();verify(a.out,a.sgx)
