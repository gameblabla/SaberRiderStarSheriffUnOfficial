#!/usr/bin/env python3
"""Extract the current game's music and master sector-aligned CD-DA tracks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import array
import math
import numpy as np
from adpcm import encode
from adpcm2 import encode as encode2, decode as decode2, tables, ROM
ROOT=Path(__file__).resolve().parents[2]
def build(out):
    work=out/'work'; music=work/'music';music.mkdir(parents=True,exist_ok=True)
    exe=work/'dcprep'
    subprocess.run(['cc','-O2','-std=c11','-Isrc','tools/dc/dcprep.c','src/pack.c','src/lzo1z.c',
                    'src/platform/common/sfx_decode.c','src/platform/common/mups.c','-o',str(exe)],cwd=ROOT,check=True)
    subprocess.run([str(exe.resolve()),'music',str(ROOT/'SaberRider/data'),str(music.resolve())],check=True)
    source=(ROOT/'src/audio.c').read_text().split('MUSIC_TABLE[18]',1)[1].split('};',1)[0]
    ids=re.findall(r'0x([0-9A-F]{8})',source)
    assert len(ids)==18
    tracks=[]
    # Three attenuated CD-DA copies: the PC Engine CD fader can only ramp to
    # silence, so the music-volume option selects a track block. Each block is
    # 18 tracks plus a silent end marker: HIGH 2-19, MID 21-38, LOW 40-57.
    levels=[('',0.9,2),('_m',0.40,21),('_l',0.16,40)]
    for suffix,gain,base in levels:
        for i,mid in enumerate(ids):
            pcm=out/f'music{i:02d}{suffix}.bin';src=music/f'{mid}.ogg'
            if not pcm.exists() or pcm.stat().st_mtime<src.stat().st_mtime:
                subprocess.run(['ffmpeg','-y','-v','error','-i',str(src),'-af',f'volume={gain},adelay=2000|2000',
                                '-ac','2','-ar','44100','-f','s16le',str(pcm)],check=True)
                with pcm.open('ab') as f:f.write(bytes(-pcm.stat().st_size%2352))
            row=dict(id=mid,logical=i,track=base+i,file=pcm.name,sectors=pcm.stat().st_size//2352)
            if suffix=='': row['sha256']=hashlib.sha256(pcm.read_bytes()).hexdigest()
            tracks.append(row)
        end=out/f'music_end{suffix}.bin';end.write_bytes(bytes(2352*300))
        tracks.append(dict(id='END',logical=-1,track=base+18,file=end.name,sectors=300))
    (out/'music_end.bin').write_bytes(bytes(2352*300))
    sfx=work/'sfx';sfx.mkdir(exist_ok=True)
    subprocess.run([str(exe.resolve()),'sfx',str(ROOT/'SaberRider/data'),str(sfx.resolve())],check=True)
    # Expand the exact Build 14 output to 5-bit DAC bytes at build time.
    # Shot/power share the code bank; impact/gallop span the three sample banks.
    t=tables();resident=bytearray();stream=bytearray();pcm_rows=[];pcm_report=[]
    for event,rid in [('shot','C66E1894'),('impact','EB3309DC'),
                      ('power','A8382083'),('gallop','82EFBA26')]:
        result=subprocess.run(['ffmpeg','-v','error','-i',str(sfx/f'{rid}.wav'),
            '-ac','1','-ar','6991','-af','highpass=f=80,lowpass=f=3000,volume=0.7',
            '-f','s16le','-'],check=True,capture_output=True)
        samples=array.array('h');samples.frombytes(result.stdout)
        if event=='shot':
            peak=max(1,max(abs(v) for v in samples))
            samples=array.array('h',(round(32767*math.tanh(2.4*v/peak)/math.tanh(2.4)) for v in samples))
        values=list(samples)+[0]*32
        packed=encode2(values,t)
        data=bytes((v+128)>>3 for v in decode2(packed,len(values),t))
        (work/f'{event}.adpcm2').write_bytes(packed)
        (work/f'{event}.dda').write_bytes(data)
        if event in ('shot','power'):
            address=f'(uint16_t)pce_pcm_resident+0x6000+{len(resident)}'
            bank=117;resident.extend(data)
        else:
            bank=125+len(stream)//8192;address=str(0xc000+len(stream)%8192)
            stream.extend(data)
        pcm_rows.append(f'{{{bank},{address},{len(values)}}}')
        pcm_report.append(dict(event=event,source=rid,samples=len(values),bytes=len(data),
                               packed_bytes=len(packed),rate=6991,
                               codec='build14-predecoded-5bit',channel=1 if event=='gallop' else 0))
    if len(stream)>3*8192:raise ValueError('DDA sample banks overflow')
    header='/* Exact Build 14 DAC bytes: bank, mapped CPU address, sample count. */\n'
    header+='const uint8_t pce_pcm_resident[] __attribute__((used,retain,section(".ram_bank117.rodata")))={'+','.join(map(str,resident))+'};\n'
    for i in range(3):
        blob=stream[i*8192:(i+1)*8192]
        header+='const uint8_t pce_pcm_bank%d[] __attribute__((used,retain,section(".ram_bank%d.rodata")))={'%(i,125+i)+','.join(map(str,blob))+'};\n'
    header+='static const uint16_t pcm_samples[4][3] __attribute__((section(".ram_bank117.rodata")))={'+','.join(pcm_rows)+'};\n'
    (out/'pcm.h').write_text(header)
    # Hardware ADPCM voice banks, one per hero (CD ADPCM RAM is 64 KiB). Events with several variants are picked at
    # random like the PC game's; every bank holds the same shared events (enemy yells, alarm, dialogue line).
    #   jump 9C7B3FD9 (sfx 2) | hurt (sfx 3, hero's own) | death (sfx 4) | fall (sfx 15, out of the level)
    #   enemy hit = sfx 5: BF4917FF BF5B14EA BF6D1599; enemy death = sfx 5 at once + sfx 6 (89389611 / 8923950D) 3 frames on
    #   alarm = sfx 22 (E105C92A, the cutscene Outrider's "!"); dialogue voice FDB525F9 (level 1 "Oh no! The Star Sheriffs!!!")
    def load(src,gain=0.65):
        result=subprocess.run(['ffmpeg','-v','error','-i',str(src),'-ac','1','-ar','8000','-af',f'volume={gain}','-f','s16le','-'],check=True,capture_output=True)
        return np.frombuffer(result.stdout,'<i2').astype(np.int32)
    def mix(first,second,delay=400):   # delay: 3 frames at 8 kHz
        out=np.zeros(max(len(first),len(second)+delay),np.int32)
        out[:len(first)]+=first;out[delay:delay+len(second)]+=second
        return np.clip(out,-32768,32767)
    yells=['BF4917FF','BF5B14EA','BF6D1599'];deaths=['89389611','8923950D']
    shared_hit=[load(sfx/f'{r}.wav') for r in yells]
    shared_death=[mix(shared_hit[i],load(sfx/f'{deaths[j]}.wav')) for i,j in ((0,0),(1,1),(2,0))]
    shared=[('enemy_hit',shared_hit,yells),('enemy_death',shared_death,[f'{yells[i]}+{deaths[j]}' for i,j in ((0,0),(1,1),(2,0))]),
            ('alarm',[load(sfx/'E105C92A.wav',0.8)],['E105C92A']),('dialogue_oh_no',[load(sfx/'FDB525F9.wav',0.8)],['FDB525F9'])]
    voices=[];rows=[]
    EVENTS=('jump','hurt','death','fall','enemy_hit','enemy_death','alarm','dialogue_oh_no')
    for hero,name in enumerate(('saber','fireball','april','colt')):
        if hero==1:   # Fireball keeps the demo's own samples: jump, the sfx 5 yells as hurt, the sfx 6 yells as death, sfx 15 for a fall
            own=[('jump',[load(sfx/'9C7B3FD9.wav')],['9C7B3FD9']),('hurt',[load(sfx/f'{r}.wav') for r in yells],yells),
                 ('death',[load(sfx/f'{r}.wav') for r in deaths],deaths),('fall',[load(sfx/'1DBF470E.wav')],['1DBF470E'])]
        else:
            def files(event,n):return [ROOT/f'assets/voice/{name}_{event}{i}.wav' if n>1 else ROOT/f'assets/voice/{name}_{event}.wav' for i in range(1,n+1)]
            own=[(e,[load(f) for f in files(e,n)],[str(f.relative_to(ROOT)) for f in files(e,n)])
                 for e,n in (('jump',1),('hurt',3),('death',2),('fall',1))]
        bank=bytearray();samples=[];groups=[]
        for event,variants,sources in sorted(own+shared,key=lambda e:EVENTS.index(e[0])):
            groups.append((len(samples),len(variants)))
            for pcm,source in zip(variants,sources):
                data=encode([0]*32+[int(v) for v in pcm]+[0]*64)
                samples.append(dict(event=event,address=len(bank),bytes=len(data),source=source))
                bank.extend(data)
        if len(bank)>65535:raise ValueError(f'{name}: ADPCM bank exceeds hardware RAM')
        total=len(bank)
        bank.extend(bytes(-len(bank)%2048));(out/f'voice{hero}.bin').write_bytes(bank)
        voices.append(dict(hero=hero,bytes=len(bank),samples=samples))
        rows.append((groups,['{%d,%d}'%(s['address'],s['bytes']) for s in samples],total))
    # The sample count per event is the same for every hero; the bank layout differs.
    counts=[g[1] for g in rows[0][0]];firsts=[g[0] for g in rows[0][0]]
    assert all([g[1] for g in r[0]]==counts and [g[0] for g in r[0]]==firsts for r in rows)
    header='/* Generated hardware ADPCM bank layout. */\n#define VOICE_SAMPLES %d\n'%len(rows[0][1])
    header+='/* per event: first sample, variants, priority */\nstatic const uint8_t voice_groups[%d][3] __attribute__((section(".ram_bank117.rodata")))={%s};\n'%(
        len(EVENTS),','.join('{%d,%d,%d}'%(f,c,pr) for f,c,pr in zip(firsts,counts,(1,2,3,3,1,1,1,4))))
    header+='static const uint16_t voice_samples[4][VOICE_SAMPLES][2] __attribute__((section(".ram_bank117.rodata")))={'+','.join('{'+','.join(r[1])+'}' for r in rows)+'};\n'
    header+='static const uint16_t voice_total[4] __attribute__((section(".ram_bank117.rodata")))={%s};\n'%','.join(str(r[2]) for r in rows)
    (out/'samples.h').write_text(header)
    (out/'audio.json').write_text(json.dumps(dict(tracks=tracks,end_track=20,volume_blocks=[2,21,40],voices=voices,dda=pcm_report),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();build(a.out.resolve())
