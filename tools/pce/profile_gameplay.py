#!/usr/bin/env python3
"""Measure completed game draws over 300 video frames in a seeded herd scene."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def profile(out):
    c=Campaign(out);elf=out/'app.elf'
    with tempfile.TemporaryDirectory(prefix='gameplay-profile-',dir=out) as base,Emulator(out/'saber_rider.cue',base) as e:
        boot(e,c.address);e.run(120)
        c.press(e,8);e.run(120)
        # Approach the first herd with no enemies/dialogs; game logic still
        # spawns/animates all horses. Held fire keeps the second voice active.
        e.write(symbol(elf,'player'),struct.pack('<4h4B',2212,160,0,0,0,0,4,4))
        c.seed(e,'camera',2092);c.seed(e,'dialogs_done',255,1)
        e.write(symbol(elf,'actors'),bytes(8*17));c.seed(e,'safe_timer',250,1)
        c.press(e,8);e.input(32)
        herd=symbol(elf,'herd_on');locked=symbol(elf,'herd_locked')
        c.until(e,lambda:e.memory(locked,1)==b'\1',limit=1000,step=1);e.input(1)
        frame=c.address+8;count=lambda:int.from_bytes(e.memory(frame,2),'little')
        before=count();e.call('prof_start')
        samples=[]
        for _ in range(10):
            c.seed(e,'safe_timer',250,1);old=count();e.run(30)
            samples.append((count()-old)&65535)
        dump=Path(base)/'prof.txt';e.call('prof_dump',str(dump))
        total=audio=0
        ranges=[((104<<13)+(symbol(elf,'pce_pcm_irq')&8191),(104<<13)+(symbol(elf,'pce_pcm_irq_end')&8191))]
        # The decoder/service is in bank 117 for both compared versions.
        for line in dump.read_text().splitlines():
            address,cycles=line.split();address=int(address,16);cycles=int(cycles);total+=cycles
            if address>>13==117 or any(lo<=address<hi for lo,hi in ranges):audio+=cycles
        report=dict(video_frames=300,game_frames=(count()-before)&65535,
                    game_frames_per_second=round(((count()-before)&65535)/5,2),
                    frames_per_30_video_frames=samples,audio_handler_percent=round(audio*100/total,2),
                    herd_active=bool(e.memory(herd,1)[0]))
        assert report['herd_active'],'Measurement must remain within the herd'
    (out/'gameplay-profile.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));profile(p.parse_args().out.resolve())
