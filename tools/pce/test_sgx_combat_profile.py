#!/usr/bin/env python3
"""Seed a walking/firing fight; measure displayed SAT generations and CPU cycles."""
import argparse,bisect,json,struct,tempfile
from profile import symbols
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def run(out,label,require_60=False):
    out=out.resolve();c=Campaign(out,sgx=True);elf=out/'app.elf'
    names=('player','actors','shots','pce_presented','pce_draws','sprite_ids',
           'pattern_owner','pattern_owner1','sprite_attr','sprite_exact','sprite_words','sprite_words1','pce_control.6','paused')
    sy={n:symbol(elf,n) for n in names}
    read=lambda e,n:int.from_bytes(e.memory(sy[n],2),'little')
    meta=json.loads((out/'manifest.json').read_text())['scenes'][0]
    blob=(out/'s1.bin').read_bytes()
    palette_checks=wide_checks=0
    def check_palettes(e):
        checked=wide=0
        ids=struct.unpack('<48H',e.memory(sy['sprite_ids'],96))
        colors=bytes.fromhex(e.call('asread','pram',16*32,16*32)['hex'])
        for vdc in (0,1):
            owners=e.memory(sy['pattern_owner1' if vdc else 'pattern_owner'],54)
            words=struct.unpack('<48H',e.memory(sy['sprite_words1' if vdc else 'sprite_words'],96))
            sat=bytes.fromhex(e.call('asread',f'sat{vdc}',0,512)['hex'])
            for y,x,pattern,attr in struct.iter_unpack('<4H',sat):
                pal=attr&15
                if pal>=12 or not 48<=y<288 or not 16<x<288:continue
                word=(pattern&2047)*32
                if not 0x4800<=word<0x7e00:continue
                owner=owners[(word-0x4800)//256]
                assert owner and ids[owner-1]<meta['sprite_count'],('live SAT cache owner',vdc,pattern,owner)
                offset=struct.unpack_from('<I',blob,meta['sprite_table']+ids[owner-1]*16+8)[0]
                assert colors[pal*32:(pal+1)*32]==blob[offset:offset+32],('live actor palette',vdc,ids[owner-1],pal)
                if attr&256:
                    assert not pattern&2,('unaligned 32-wide sprite',pattern)
                    index=(word-words[owner-1])//64
                    patterns=struct.unpack_from('<I',blob,meta['sprite_table']+ids[owner-1]*16)[0]
                    actual=bytes.fromhex(e.call('asread',f'vram{vdc}',word*2,256)['hex'])
                    assert actual==blob[patterns+index*128:patterns+(index+2)*128],('wide sprite patterns',vdc,ids[owner-1],index)
                    wide+=1
                checked+=1
        return checked,wide

    with tempfile.TemporaryDirectory(prefix='combat-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);e.run(120);c.seed(e,'dialogs_done',255,1)
        c.move(e,600);c.press(e,8);e.run(120)
        e.write(sy['shots'],bytes(208))
        rows=[]
        for i in range(6):
            rows.append(struct.pack('<4h4B9B',640+i*28,160,0,0,0,0,4,4,1,[1,2,5,6,7,9][i],20,i*4,1,0,i*3,0,0))
        e.write(sy['actors'],b''.join(rows)+bytes(42));c.seed(e,'safe_timer',250,1)
        c.press(e,8);e.input(33)
        before=read(e,'pce_presented');m=c.metrics(e);e.call('prof_start')
        samples=[];actors=[];trace=[]
        table=symbols(elf);starts=[a for a,_ in table]
        for i in range(180):
            c.seed(e,'safe_timer',250,1)
            e.input(1|(32 if (i//30)%2==0 else 128));old=read(e,'pce_presented');e.run(1)
            samples.append((read(e,'pce_presented')-old)&65535)
            actors.append(sum(e.memory(sy['actors'],168)[12::21]))
            if i<60:
                regs=e.call('registers')['registers'];pc=regs['PC'];physical=regs[f'MPR{pc>>13}']*8192+(pc&8191)
                name=table[bisect.bisect_right(starts,physical)-1][1]
                trace.append(dict(video_frame=i+1,presented=samples[-1],function=name,pc=pc,
                    elapsed=e.memory(sy['pce_control.6'],1)[0],paused=e.memory(sy['paused'],1)[0],
                    actors=[list(struct.unpack_from('<4h4B9B',e.memory(sy['actors'],168),n*21)) for n in range(8)]))
            if i==29:e.call('prof_dump',str(out/f'combat-{label}-first30-cycles.txt'))
            if i%15==14:
                checked,wide=check_palettes(e);palette_checks+=checked;wide_checks+=wide
        (out/f'combat-{label}-trace.json').write_text(json.dumps(trace,indent=2)+'\n')
        log=out/f'combat-{label}-cycles.txt';e.call('prof_dump',str(log))
        e.screenshot(out/f'combat-{label}.png')
        report=dict(video_frames=180,presented=(read(e,'pce_presented')-before)&65535,
            fps=round(sum(samples)/3,2),active_actors_min=min(actors),active_actors_max=max(actors),
            upload_bytes=(c.metrics(e)['uploads']-m['uploads'])&65535,
            overflow=c.metrics(e)['essential_overflow']-m['essential_overflow'],
            admission='scanlines' if e.memory(sy['sprite_exact'],1)[0] else 'bands',
            presentations_per_30_frames=[sum(samples[n:n+30]) for n in range(0,180,30)],
            wide_parts_checked=wide_checks,palette_parts_checked=palette_checks,missed_video_frames=samples.count(0),presentations_per_video_frame={str(n):samples.count(n) for n in sorted(set(samples))})
        (out/f'combat-{label}.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
        assert palette_checks>=20,'Must check displayed native actor palettes'
        assert max(actors)>=4 and min(actors)>=3,'Must exercise multiple native enemies'
        if require_60:
            assert samples==[1]*180,'Every video frame must present one new SAT generation'
            assert report['overflow']==0,'Essential sprites must fit'
        if label=='after':assert report['fps']>=32,'Combat cadence regressed below the optimized fixture floor'
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));p.add_argument('--label',default='after')
    p.add_argument('--require-60',action='store_true')
    a=p.parse_args();run(a.out,a.label,a.require_60)
