#!/usr/bin/env python3
"""Capture native audio and check PCM IRQ lifetime and voice-only events.

After ordinary disc boot, RAM trampolines call compiled audio_stop/audio_effect.
This isolates samples from music/gameplay. PCM delivery and ADPCM playback run
on the accurate emulated hardware, with the game's IRQs still installed.
"""
import argparse
import json
from pathlib import Path
import tempfile
from urllib.parse import quote
import wave
import numpy as np
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out):
    elf=out/'app.elf';metrics=symbol(elf,'pce_metrics');effect=symbol(elf,'audio_effect');stop=symbol(elf,'audio_stop')
    voices=symbol(elf,'pce_pcm_voices');reports={}
    try:psg_live_addr=symbol(elf,'psg_live')
    except Exception:psg_live_addr=None
    def left(e,ch=0):return int.from_bytes(e.memory(voices+ch*16,2),'little')
    recordings=out/'audio-review';recordings.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='audio-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,metrics);e.run(120)
        def call(e,address,arg=0):
            code=bytes([0xa9,arg,0x20,address&255,address>>8,0x4c,0xf5,0x3b])
            e.write(0x3bf0,code)
            # A video boundary may pause within an IRQ. Start the isolated
            # call with a clean CPU context rather than abandoning its I bit
            # and temporary mappings in the middle of an interrupt.
            for key,value in [('P',0),('SP',253),('MPR3',105),('MPR6',108),('PC',0x3bf0)]:
                e.call('register_set',key,value)
            e.run(1)
        def capture(e,name,frames):
            path=recordings/(name+'.wav');e.call('sound_capture',frames,quote(str(path.resolve()),safe='/'))
            with wave.open(str(path),'rb') as w:pcm=np.frombuffer(w.readframes(w.getnframes()),'<i2').reshape(-1,2)
            return dict(file=str(path.relative_to(out)),peak=int(np.abs(pcm.astype(np.int32)).max()),
                rms=float(np.sqrt(np.mean(pcm.astype(np.float64)**2))),
                tail_rms=float(np.sqrt(np.mean(pcm[-4410:].astype(np.float64)**2))))
        for tone,name in [(1,'shoot'),(2,'jump'),(5,'hurt'),(6,'death'),(7,'enemy_hit'),(8,'enemy_death'),(9,'alarm'),(10,'dialogue_oh_no'),(11,'fall'),(4,'impact')]:
            call(e,stop);e.run(120)
            quiet=capture(e,'quiet-'+name,12);assert quiet['rms']<10,quiet
            saved_bank=e.call('registers')['registers']['MPR6'];call(e,effect,tone)
            registers=e.call('registers')['registers']
            if tone==1:
                # Hero's shot is pure PSG (psg_pce.c, script 0 on voice 0): no
                # DDA timer, no ADPCM. Isolated trampoline calls cannot advance
                # its 1/60 s envelope (audio_tick runs in the main loop), so
                # sound_capture would stay silent here; just check it starts
                # cleanly without touching DDA/ADPCM.
                assert registers['TIMS']==0 and not any(left(e,ch) for ch in range(2)),'Shot must not use DDA'
                assert registers['Playing']==0,'Shot must not use ADPCM'
                assert psg_live_addr is not None and e.memory(psg_live_addr,1)[0]&1,'Shot must start PSG voice 0'
                reports[name]=dict(psg=True,note='pure PSG, verified in-game via stage-card capture')
                continue
            if tone in (4,):
                channel=0
                remaining=left(e,channel)
                assert registers['TIMS']==1 and remaining>0
                assert registers['MPR6']==saved_bank,'PCM IRQ must restore interrupted MPR6'
                assert e.memory(voices+channel*16+9,1)[0]==125
                before=remaining;e.run(1);after=left(e,channel)
                delivered=before-after
                assert 110<=delivered<=122,(before,after)
                reports[name]=dict(delivered_per_video_frame=delivered,**capture(e,name,120))
                assert not any(left(e,ch) for ch in range(2))
                assert e.call('registers')['registers']['TIMS']==0,'One-shot PCM must stop its timer'
            else:
                assert registers['Playing']==1,'Voice event must start ADPCM playback'
                assert registers['TIMS']==0 and not any(left(e,ch) for ch in range(2)),'No synthesized jump/death tone'
                reports[name]=capture(e,name,180)
            assert reports[name]['rms']>20,reports[name]
            assert reports[name]['tail_rms']<10,reports[name]
        # Every variant of a voice event is reachable (the PC game picks hurt / death / yells at random).
        voice=symbol(elf,'pce_voice.0');variants={}   # LTO splits the struct: .0 is the ADPCM address
        for tone,count in [(5,3),(6,2),(7,3),(8,3),(9,1),(10,1),(11,1)]:
            seen=set()
            for _ in range(40):
                call(e,effect,tone);seen.add(int.from_bytes(e.memory(voice,2),'little'));call(e,stop)
            assert len(seen)==count,(tone,seen)
            variants[tone]=len(seen)
        reports['voice_variants']=variants
        # Stage-card / menu tick (sfx 0) is pure PSG script 6, no DDA/ADPCM.
        tick=symbol(elf,'audio_pcm_tick')
        call(e,stop);e.run(30);call(e,tick)
        assert e.call('registers')['registers']['TIMS']==0,'Tick must not use DDA'
        assert e.call('registers')['registers']['Playing']==0,'Tick must not use ADPCM'
        assert not any(left(e,ch) for ch in range(2)),'Tick must not touch DDA voices'
        assert psg_live_addr is not None and e.memory(psg_live_addr,1)[0]&1,'Tick must start PSG'
        reports['tick_psg']=dict(psg=True)
        call(e,stop)
        # Stop PSG shot while active, as every loader stops all audio before CD ops.
        call(e,effect,1);assert e.call('registers')['registers']['TIMS']==0
        assert psg_live_addr is not None and e.memory(psg_live_addr,1)[0]&1
        call(e,stop);assert not any(left(e,ch) for ch in range(2)) and e.call('registers')['registers']['TIMS']==0
        assert psg_live_addr is None or e.memory(psg_live_addr,1)[0]==0,'audio_stop must silence PSG'
        reports['stop_during_shot']=capture(e,'stop-during-shot',30)
        assert reports['stop_during_shot']['tail_rms']<10
        # Two concurrent DDA voices: the gallop must survive one-shot impacts.
        # (The hero's shot is independent pure PSG.) Stopping gallop must
        # leave one-shot DDA running.
        gallop=symbol(elf,'audio_pcm_gallop')
        call(e,stop);e.run(120);call(e,gallop,1);call(e,effect,1);call(e,effect,4)
        assert all(left(e,ch)>0 for ch in range(2))
        assert e.memory(voices+9,1)[0]==125,'Impact on DDA channel 0'
        assert e.memory(voices+16+9,1)[0] in (125,126,127,132),'Gallop keeps its independent DDA channel'
        before=[left(e,ch) for ch in range(2)]
        e.run(1)
        after=[left(e,ch) for ch in range(2)]
        assert all(110<=a-b<=122 for a,b in zip(before,after)),(before,after)
        # Frame boundaries can fall inside the IRQ. Check mappings at the
        # idle trampoline, after a complete interrupt return.
        for key,value in [('MPR3',105),('MPR6',108)]:e.call('register_set',key,value)
        e.call('register_set','SP',253);e.call('register_set','PC',0x3bf5)
        for _ in range(60):
            e.run(1);now=e.call('registers')['registers']
            if now['PC']==0x3bf5:break
        assert now['PC']==0x3bf5 and now['MPR3']==105 and now['MPR6']==108,now
        reports['concurrent']=capture(e,'concurrent',12)
        call(e,gallop,0);assert left(e,1)==0 and left(e,0)>0
        call(e,gallop,1);e.run(180)
        assert left(e,1)>0 and left(e,0)==0
        reports['gallop_loop']=capture(e,'gallop-loop',60)
        assert reports['gallop_loop']['rms']>20
        call(e,stop);reports['stop_gallop']=capture(e,'stop-gallop',30)
        assert reports['stop_gallop']['tail_rms']<10
    # Other heroes share the same ADPCM playback path (verified above on Saber);
    # their banks differ only in data. Check sizes against the 64 KiB CD RAM.
    import json as _json
    _audio=_json.load(open(out/'audio.json'))
    for v in _audio['voices']:
        assert v['bytes']<=65536,(v['hero'],v['bytes'])
    reports['voice_bank_bytes']={str(v['hero']):v['bytes'] for v in _audio['voices']}
    (out/'audio-verification.json').write_text(json.dumps(reports,indent=2)+'\n');print(json.dumps(reports,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));a=p.parse_args();verify(a.out.resolve())
