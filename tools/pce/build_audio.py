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
    voices=[];rows=[]
    for hero,name in enumerate(('saber','fireball','april','colt')):
        bank=bytearray();samples=[]
        # Events 3/4 are the enemies' hit and death yells (game sfx 5 and 6), shared by every hero bank.
        for event,rid in zip(('jump','hurt1','death1','enemy_hit','enemy_death'),('9C7B3FD9','BF4917FF','89389611','BF5B14EA','8923950D')):
            src=sfx/f'{rid}.wav' if hero==1 or event.startswith('enemy') else ROOT/f'assets/voice/{name}_{event}.wav'
            result=subprocess.run(['ffmpeg','-v','error','-i',str(src),'-ac','1','-ar','8000','-af','volume=0.65','-f','s16le','-'],check=True,capture_output=True)
            pcm=array.array('h');pcm.frombytes(result.stdout)
            data=encode([0]*32+list(pcm)+[0]*64)
            samples.append(dict(event=event,address=len(bank),bytes=len(data),source=str(src.relative_to(ROOT))))
            bank.extend(data)
        if len(bank)>65535:raise ValueError(f'{name}: ADPCM bank exceeds hardware RAM')
        bank.extend(bytes(-len(bank)%2048));(out/f'voice{hero}.bin').write_bytes(bank)
        voices.append(dict(hero=hero,bytes=len(bank),samples=samples))
        rows.append('{'+','.join('{%d,%d}'%(s['address'],s['bytes']) for s in samples)+'}')
    (out/'samples.h').write_text('/* Generated hardware ADPCM bank offsets. */\nstatic const uint16_t voice_samples[4][5][2]={'+','.join(rows)+'};\n')
    (out/'audio.json').write_text(json.dumps(dict(tracks=tracks,end_track=20,volume_blocks=[2,21,40],voices=voices,dda=pcm_report),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();build(a.out.resolve())
