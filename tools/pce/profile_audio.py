#!/usr/bin/env python3
"""Measure playback service cycles for 1/2 software voices using native timer IRQs.

Uses isolated RAM calls after normal boot. service_percent excludes the resident wrapper; handler_percent includes it.
Both exclude BIOS dispatch. This does not measure gameplay rendering throughput.
"""
import argparse
import json
from pathlib import Path
import tempfile
from emulator import Emulator,boot,symbol

def profile(out):
    reports=[];elf=out/'app.elf';state=symbol(elf,'pce_pcm_voices')
    irq_start=(104<<13)+(symbol(elf,'pce_pcm_irq')&8191)
    irq_end=(104<<13)+(symbol(elf,'pce_pcm_irq_end')&8191)
    service_start=(107<<13)+(symbol(elf,'pce_pcm_service')&8191)
    service_end=(107<<13)+(symbol(elf,'pce_pcm_service_end')&8191)
    with tempfile.TemporaryDirectory(prefix='audio-profile-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,symbol(elf,'pce_metrics'));e.run(120)
        def call(name,arg=0):
            a=symbol(elf,name)
            e.write(0x3b00,bytes([0xa9,arg,0x20,a&255,a>>8,0x4c,5,0x3b]))
            for key,value in [('P',0),('SP',253),('MPR3',105),('MPR6',108),('PC',0x3b00)]:
                e.call('register_set',key,value)
            e.run(1)
        for n in range(1,3):
            call('audio_stop');e.run(120);call('audio_pcm_gallop',1)
            if n>=2:call('audio_effect',1)
            if n==2:
                call('audio_effect',5)
                assert e.call('registers')['registers']['Playing']==1
                assert all(e.memory(state+i*16,2)!=b'\0\0' for i in range(2))
            e.call('prof_start');e.run(12)
            dump=Path(base)/'prof.txt';e.call('prof_dump',str(dump))
            total=service=wrapper=0
            for line in dump.read_text().splitlines():
                address,cycles=line.split();address=int(address,16);cycles=int(cycles);total+=cycles
                if service_start<=address<service_end:service+=cycles
                if irq_start<=address<irq_end:wrapper+=cycles
            reports.append(dict(channels=n,video_frames=12,service_percent=round(service*100/total,2),
                                handler_percent=round((service+wrapper)*100/total,2)))
    (out/'audio-profile.json').write_text(json.dumps(reports,indent=2)+'\n');print(reports)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));profile(p.parse_args().out.resolve())
