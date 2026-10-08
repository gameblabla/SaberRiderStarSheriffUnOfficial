#!/usr/bin/env python3
"""Measure completed game draws over 300 video frames in a seeded herd scene."""
import argparse,collections,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def profile(out,herd_number=1,require_60=False,sgx=False):
    c=Campaign(out,sgx=sgx);elf=out/'app.elf'
    meta=json.loads((out/'work/stage1.json').read_text())
    trigger=[t for t in meta['triggers'] if t['type']==11][herd_number-1]
    x=trigger['zone'][0]-40
    with tempfile.TemporaryDirectory(prefix='gameplay-profile-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=sgx) as e:
        boot(e,c.address);e.run(120)
        c.press(e,8);e.run(120)
        # Approach the selected herd with no enemies/dialogs; game logic still
        # spawns/animates all horses. Held fire keeps the second voice active.
        e.write(symbol(elf,'player'),struct.pack('<4h4B',x,160,0,0,0,0,4,4))
        c.seed(e,'camera',x-120);c.seed(e,'dialogs_done',255,1)
        e.write(symbol(elf,'actors'),bytes(8*21));c.seed(e,'safe_timer',250,1)
        c.press(e,8);e.input(32)
        herd=symbol(elf,'herd_on');locked=symbol(elf,'herd_locked')
        def reached():
            c.seed(e,'safe_timer',250,1);return e.memory(locked,1)==b'\1'
        c.until(e,reached,limit=1000,step=1)
        frame=symbol(elf,'pce_draws');count=lambda:int.from_bytes(e.memory(frame,2),'little')
        # Lock is set inside play_tick, before its scrolling draw completes.
        # Start at a completed locked-scene draw, then begin firing immediately.
        lock_draw=count();c.until(e,lambda:count()!=lock_draw,limit=20,step=1);e.input(1)
        presented=symbol(elf,'pce_presented');visible=lambda:int.from_bytes(e.memory(presented,2),'little')
        visible_before=visible();display_cadence=[]
        metrics_before=c.metrics(e)
        cache_events=[];upload_previous=metrics_before['uploads']
        ids_address=symbol(elf,'sprite_ids')
        before=count();e.call('prof_start')
        samples=[];cadence=[]
        for _ in range(10):
            c.seed(e,'safe_timer',250,1);old=count()
            for _ in range(30):
                previous=count();shown=visible();e.run(1);cadence.append((count()-previous)&65535)
                display_cadence.append((visible()-shown)&65535)
                uploaded=c.metrics(e)['uploads']
                if uploaded!=upload_previous:
                    cache_events.append(dict(video_frame=len(cadence),bytes=(uploaded-upload_previous)&65535,
                        ids=list(struct.unpack('<48H',e.memory(ids_address,96)))))
                    upload_previous=uploaded
            samples.append((count()-old)&65535)
        dump=Path(base)/'prof.txt';e.call('prof_dump',str(dump))
        (out/f'gameplay-cycles-herd{herd_number}.txt').write_text(dump.read_text())
        (out/f'gameplay-cache-events-herd{herd_number}.json').write_text(json.dumps(cache_events,indent=2)+'\n')
        total=audio=0
        ranges=[((104<<13)+(symbol(elf,'pce_pcm_irq')&8191),(104<<13)+(symbol(elf,'pce_pcm_irq_end')&8191))]
        ranges.append(((107<<13)+(symbol(elf,'pce_pcm_service')&8191),(107<<13)+(symbol(elf,'pce_pcm_service_end')&8191)))
        for line in dump.read_text().splitlines():
            address,cycles=line.split();address=int(address,16);cycles=int(cycles);total+=cycles
            if any(lo<=address<hi for lo,hi in ranges):audio+=cycles
        report=dict(herd=herd_number,video_frames=300,game_frames=(count()-before)&65535,
                    game_frames_per_second=round(((count()-before)&65535)/5,2),
                    frames_per_30_video_frames=samples,audio_handler_percent=round(audio*100/total,2),
                    draws_per_video_frame=dict(sorted(collections.Counter(cadence).items())),
                    presented_frames=(visible()-visible_before)&65535,
                    presentations_per_video_frame=dict(sorted(collections.Counter(display_cadence).items())),
                    essential_overflows=c.metrics(e)['essential_overflow']-metrics_before['essential_overflow'],
                    sprite_upload_bytes=(c.metrics(e)['uploads']-metrics_before['uploads'])&65535,
                    herd_active=bool(e.memory(herd,1)[0]))
        assert report['herd_active'],'Measurement must remain within the herd'
    (out/f'gameplay-profile-herd{herd_number}.json').write_text(json.dumps(report,indent=2)+'\n')
    if herd_number==1:(out/'gameplay-profile.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report)
    if require_60:
        assert report['presentations_per_video_frame']=={1:300},'Every VBlank must present a new SAT'
        assert report['essential_overflows']==0,'Essential graphics must fit'
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'))
    p.add_argument('--herd',type=int,choices=[1,2,3],default=1);p.add_argument('--require-60',action='store_true')
    p.add_argument('--sgx',action='store_true')
    args=p.parse_args();profile(args.out.resolve(),args.herd,args.require_60,args.sgx)
