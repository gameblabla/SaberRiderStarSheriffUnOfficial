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
    # Native PSG effects (psg_pce.c): the hero's shot (C66E1894), the flying bosses' cannon (8AEB8147) and rider's gun (0AFC505A) are rebuilt from the
    # source recordings as a noise channel (its level and pitch follow the recording's loudness and brightness, one step every 1/60 s) with a low
    # tone under it (the recordings' 150-400 Hz body), so no DDA or ADPCM channel is needed and nothing cuts them off.
    import wave
    def psg_script(rid,tone_db,limit,noise_db):
        """One PSG step (1/60 s) per row from the recording itself: the tone follows the strongest partial of the 80-1300 Hz body (its pitch and loudness),
        the noise follows what is above 1300 Hz (its loudness, and a clock from its brightness: the noise generator's rate is 7.16 MHz / (128 (31 - n)))."""
        with wave.open(str(sfx/f'{rid}.wav')) as w:
            d=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(np.float64);sr=w.getframerate()
            if w.getnchannels()>1:d=d.reshape(-1,w.getnchannels()).mean(1)
        step=int(sr/60);win=step*2;fq=np.fft.rfftfreq(4096,1/sr);frames=[]
        low=(fq>=80)&(fq<1300);high=(fq>=1300)&(fq<6000)
        for i in range(0,len(d)-win,step):
            seg=d[i:i+win]*np.hanning(win);sp=np.abs(np.fft.rfft(seg,4096))**2
            lo_e=sp[low].sum();hi_e=sp[high].sum()
            k=np.argmax(sp*low);pk=fq[k]
            if 1<k<len(sp)-2:   # parabolic peak
                a1,b1,c1=np.log(sp[k-1]+1e-9),np.log(sp[k]+1e-9),np.log(sp[k+1]+1e-9);dlt=0.5*(a1-c1)/(a1-2*b1+c1-1e-12);pk=fq[k]+dlt*(fq[1]-fq[0])
            cen=(sp[high]*fq[high]).sum()/(hi_e+1e-9)
            frames.append((np.sqrt(lo_e),np.sqrt(hi_e),pk,cen))
        peak=max(max(l,h) for l,h,_,_ in frames);rows=[];prev_pk=300.0
        for l,h,pk,cen in frames[:limit]:
            tv=max(0,min(31,round(31+20*math.log10(max(l,1)/peak)/1.5+tone_db/1.5)))
            nv=max(0,min(31,round(31+20*math.log10(max(h,1)/peak)/1.5+noise_db/1.5)))
            pk=0.5*pk+0.5*prev_pk if abs(pk-prev_pk)<60 else pk;prev_pk=pk   # (no smoothing across a jump of the pitch)
            nf=max(0,min(30,31-round(13984/max(cen,400))))
            fr=max(1,min(4095,round(3579545/(32*max(60.0,pk)))))
            rows.append((nv,nf,tv,fr&255,fr>>8))
        while rows and rows[-1][0]<3 and rows[-1][2]<3:rows.pop()
        rows.append((255,0,0,0,0))
        return rows
    psg_rows=[psg_script('C66E1894',-3,30,-3),psg_script('8AEB8147',0,26,-5),psg_script('0AFC505A',0,24,-5)]
    ph='/* Generated: native PSG effects, 5 bytes a 1/60 s step: noise level, noise clock, tone level, tone period (16 bits); 255 ends. */\n'
    ph+='static const uint8_t psg_scripts[%d][%d] __attribute__((section(".ram_bank117.rodata")))={'%(len(psg_rows),max(len(r) for r in psg_rows)*5)
    ph+=','.join('{'+','.join(str(v) for row in r for v in row)+'}' for r in psg_rows)+'};\n'
    (out/'psg.h').write_text(ph)
    # Expand the exact Build 14 output to 5-bit DAC bytes at build time.
    # Shot/power share the code bank; impact/gallop span the three sample banks.
    t=tables();resident=bytearray();stream=bytearray();pcm_rows=[];pcm_report=[]
    for event,rid in [('shot','C66E1894'),('impact','87265BA0'),
                      ('power','A8382083'),('gallop','82EFBA26'),('tick','E418A101')]:
        if event=='shot':pcm_rows.append('{0,0,0}');continue   # the hero's shot is native PSG now (psg_pce.c, psg.h below): no DDA sample, no bank space
        if event=='tick':pcm_rows.append('{0,0,0}')   # request 4 stops the gallop: no sample 4
        # Keep the hoof rumble audible beside CD-DA; the general effects filter
        # removed its bass and attenuated an already shared PSG mix.
        filtering='highpass=f=20,lowpass=f=3000,volume=1.0' if event=='gallop' else 'highpass=f=80,lowpass=f=3000,volume=0.7'
        result=subprocess.run(['ffmpeg','-v','error','-i',str(sfx/f'{rid}.wav'),
            '-ac','1','-ar','6991','-af',filtering,
            '-f','s16le','-'],check=True,capture_output=True)
        samples=array.array('h');samples.frombytes(result.stdout)
        # as loud as the 5-bit DAC allows without clipping: the peak goes to 95 percent of full scale (the sources were mastered at very different levels)
        peak=max(1,max(abs(v) for v in samples))
        samples=array.array('h',(max(-32767,min(32767,round(v*0.95*32767/peak))) for v in samples))
        if event=='tick':
            # The CONTINUE? countdown's tick (the PC game's sfx 0): trimmed, with a short fade, to what the sample banks have left.
            room=3*8192-len(stream)-32
            if len(samples)>room:
                samples=samples[:room];fade=min(256,room)
                for k in range(fade):samples[room-1-k]=int(samples[room-1-k]*k/fade)
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
    header+='static const uint16_t pcm_samples[6][3] __attribute__((section(".ram_bank117.rodata")))={'+','.join(pcm_rows)+'};\n'
    (out/'pcm.h').write_text(header)
    # Hardware ADPCM voice banks, one per hero (CD ADPCM RAM is 64 KiB). Events with several variants are picked at
    # random like the PC game's; every bank holds the same shared events (enemy yells, alarm, dialogue line).
    #   jump 9C7B3FD9 (sfx 2) | hurt (sfx 3, hero's own) | death (sfx 4) | fall (sfx 15, out of the level)
    #   (the PCM 'impact' is the source's sfx 14, the grenade's burst: the sfx 4 death sample is the hero's "ugh" and plays only for Fireball's death)
    #   enemy hit = sfx 5: BF4917FF BF5B14EA BF6D1599; enemy death = sfx 5 at once + sfx 6 (89389611 / 8923950D) 3 frames on
    #   alarm = sfx 22 (E105C92A, the cutscene Outrider's "!"); dialogue voice FDB525F9 (level 1 "Oh no! The Star Sheriffs!!!")
    def load(src,gain=0.65,rate=8000,seconds=None,fade=0.0):
        # the CD ADPCM player runs at 32 kHz / (16 - n): n=12 is 8 kHz, 10 is 5.33 kHz, 8 is 4 kHz
        filters=f'lowpass=f={min(3500,rate*0.45):.0f},volume={gain}'
        if seconds:filters+=f',atrim=end={seconds}'
        if fade:filters+=f',afade=t=out:st={seconds-fade}:d={fade}'
        result=subprocess.run(['ffmpeg','-v','error','-i',str(src),'-ac','1','-ar',str(rate),'-af',filters,'-f','s16le','-'],check=True,capture_output=True)
        return loud(np.frombuffer(result.stdout,'<i2').astype(np.int32),gain)
    def loud(pcm,level):
        # Peak-normalised, so every effect is as loud as the ADPCM allows without clipping (the 4-bit coder overshoots a little: 88 percent of full
        # scale at most, scaled by the event's own relative level, 1.0 for a voice) instead of whatever level the source happened to be mastered at.
        peak=max(1,int(np.abs(pcm).max()))
        return np.clip(pcm.astype(np.float64)*(0.88*32767*min(1.0,level/0.65)/peak),-32767,32767).astype(np.int32)
    def mix(first,second,delay=400):   # delay: 3 frames at 8 kHz
        out=np.zeros(max(len(first),len(second)+delay),np.int32)
        out[:len(first)]+=first;out[delay:delay+len(second)]+=second
        return loud(out,0.65)
    yells=['BF4917FF','BF5B14EA','BF6D1599'];deaths=['89389611','8923950D']
    shared_hit=[load(sfx/f'{r}.wav') for r in yells]
    shared_death=[mix(shared_hit[i],load(sfx/f'{deaths[j]}.wav')) for i,j in ((0,0),(1,1),(2,0))]
    shared=[('enemy_hit',shared_hit,yells),('enemy_death',shared_death,[f'{yells[i]}+{deaths[j]}' for i,j in ((0,0),(1,1),(2,0))]),
            ('alarm',[load(sfx/'E105C92A.wav',0.8)],['E105C92A']),('dialogue_oh_no',[load(sfx/'FDB525F9.wav',0.8)],['FDB525F9'])]
    # The flying bosses' sounds (the source's sfx 0x13 engine pass, 0x10 gun, 0x12 rider's gun, 0x11 blast, 0x15 the wreck's big bang),
    # at the lower ADPCM rates that fit beside a hero's bank in the 64 KiB; the gun and the rider's gun also as the pair the
    # gunship fires on the same step.
    gun=load(sfx/'8AEB8147.wav',0.9,5333);pilot=load(sfx/'0AFC505A.wav',0.9,5333)
    pair=np.zeros(max(len(gun),len(pilot)),np.int32);pair[:len(gun)]+=gun;pair[:len(pilot)]+=pilot;pair=loud(pair,0.65)
    shared+=[('boss_appear',[load(sfx/'15A00BA1.wav',0.9,4000,4.2,0.5)],['15A00BA1']),
             ('boss_cannon',[gun],['8AEB8147']),('boss_rider',[pilot],['0AFC505A']),
             ('boss_volley',[pair],['8AEB8147+0AFC505A']),
             ('boss_blast',[load(sfx/'F11FCC31.wav',0.9,5333)],['F11FCC31']),
             ('boss_down',[load(sfx/'47D886A1.wav',0.9,4000,3.4,0.4)],['47D886A1'])]
    voices=[];rows=[]
    EVENTS=('jump','hurt','death','fall','enemy_hit','enemy_death','alarm','dialogue_oh_no','boss_appear','boss_cannon','boss_rider','boss_volley','boss_blast','boss_down')
    PRIORITY=(1,2,3,3,1,1,1,4,1,0,0,0,0,2)
    RATE=(12,12,12,12,12,12,12,12,8,10,10,10,10,8)
    for hero,name in enumerate(('saber','fireball','april','colt')):
        if hero==1:   # Fireball keeps the demo's own samples: jump, sfx 3 as hurt, sfx 4 as death, sfx 15 for a fall
            hurts=['8ADE82B6','8ACB81A0','8AB88092'];own_deaths=['EB3309DC','EB450AED']   # the PC game's own: sfx 3 (hurt), sfx 4 (death: the "ugh")
            own=[('jump',[load(sfx/'9C7B3FD9.wav')],['9C7B3FD9']),('hurt',[load(sfx/f'{r}.wav') for r in hurts],hurts),
                 ('death',[load(sfx/f'{r}.wav') for r in own_deaths],own_deaths),('fall',[load(sfx/'1DBF470E.wav')],['1DBF470E'])]
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
    header+='/* per event: first sample, variants, priority, BIOS ADPCM rate */\nstatic const uint8_t voice_groups[%d][4] __attribute__((section(".ram_bank117.rodata")))={%s};\n'%(
        len(EVENTS),','.join('{%d,%d,%d,%d}'%(f,c,pr,rt) for f,c,pr,rt in zip(firsts,counts,PRIORITY,RATE)))
    header+='static const uint16_t voice_samples[4][VOICE_SAMPLES][2] __attribute__((section(".ram_bank117.rodata")))={'+','.join('{'+','.join(r[1])+'}' for r in rows)+'};\n'
    header+='static const uint16_t voice_total[4] __attribute__((section(".ram_bank117.rodata")))={%s};\n'%','.join(str(r[2]) for r in rows)
    (out/'samples.h').write_text(header)
    (out/'audio.json').write_text(json.dumps(dict(tracks=tracks,end_track=20,volume_blocks=[2,21,40],voices=voices,dda=pcm_report),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();build(a.out.resolve())
