#!/usr/bin/env python3
"""Exercise compiled CD-DA completion with short and padded lead-out replies."""
import argparse,json,tempfile,wave
from pathlib import Path
import numpy as np
from emulator import Emulator,boot,symbol


def verify(out,sgx):
    out=out.resolve();elf=out/'app.elf'
    names='pce_metrics audio_stop audio_tick complete command status received lead_out last_track active attempts pce_music_status'.split()
    a={n:symbol(elf,n) for n in names}
    # CUE audio files have no gaps after the last track. Disc info in the
    # emulator provides the mastered lead-out via the real native cue first.
    with tempfile.TemporaryDirectory(prefix='leadout-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=sgx) as e:
        boot(e,a['pce_metrics'])
        def invoke(address,bank=105):
            tick=a['audio_tick'];loop=0x3bf3
            e.write(0x3bf0,bytes([0x20,address&255,address>>8,
                0x20,tick&255,tick>>8,0x4c,loop&255,loop>>8]))
            for key,value in [('P',0),('SP',253),('MPR3',bank),('MPR6',108),('PC',0x3bf0)]:
                e.call('register_set',key,value)
        # Obtain the exact native TOC MSF through a normal final-track request.
        start=symbol(elf,'audio_music')
        e.write(0x3bf0,bytes([0xa9,17,0x20,start&255,start>>8,0x4c,0xf8,0x3b,
                           0x20,a['audio_tick']&255,a['audio_tick']>>8,0x4c,0xf8,0x3b]))
        for key,value in [('P',0),('SP',253),('MPR3',105),('MPR6',108),('PC',0x3bf0)]:
            e.call('register_set',key,value)
        e.run(600)
        assert e.memory(a['pce_music_status'],1)==b'\0'
        lead=e.memory(a['lead_out'],3)
        results={}
        for length in (3,4):
            invoke(a['audio_stop']);e.run(180)
            # A completed DE response with three MSF bytes and optional pad.
            # The controller has already released the bus, as in the supplied
            # state (command DE, received 3, protocol error after 3 attempts).
            for name,value in [('command',0xde),('status',0),('received',length),('last_track',1),('active',0),('attempts',3)]:
                e.write(a[name],bytes([value]))
            e.write(a['lead_out'],lead)
            invoke(a['complete'],123);e.run(180)
            assert e.memory(a['pce_music_status'],1)==b'\0',(length,'lead-out reply rejected')
            assert e.memory(a['active'],1)==b'\1',(length,'final track never started')
            path=out/f'cdda-leadout-{length}.wav';e.call('sound_capture',120,str(path))
            with wave.open(str(path)) as wav:
                pcm=np.frombuffer(wav.readframes(wav.getnframes()),'<i2').astype(float)
            rms=float(np.sqrt(np.mean(pcm*pcm)))
            assert rms>100,(length,'silent final track',rms)
            results[str(length)]=rms
        (out/'cdda-leadout-verification.json').write_text(json.dumps(results,indent=2)+'\n')
        print('Native lead-out replies passed',results,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));p.add_argument('--sgx',action='store_true')
    args=p.parse_args();verify(args.out,args.sgx)
