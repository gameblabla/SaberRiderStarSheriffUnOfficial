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
from adpcm2 import encode as encode2, tables, ROM
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
    # Build 14 software ADPCM: two independent PSG DDA voices, timer at 6991 Hz.
    # Each packed sample stays in one bank. Code and exact ROM tables occupy dedicated bank $75 below the sample banks.
    # The loader scratch is moved to $76-$7c to preserve the decoder.
    t=tables();banks=[bytearray() for _ in range(3)];pcm_rows=[];pcm_report=[]
    for event,rid,bank in [('shot','C66E1894',125),('impact','EB3309DC',126),
                           ('power','A8382083',125),('gallop','82EFBA26',127)]:
        result=subprocess.run(['ffmpeg','-v','error','-i',str(sfx/f'{rid}.wav'),
            '-ac','1','-ar','6991','-af','highpass=f=80,lowpass=f=3000,volume=0.7',
            '-f','s16le','-'],check=True,capture_output=True)
        samples=array.array('h');samples.frombytes(result.stdout)
        if event=='shot':
            peak=max(1,max(abs(v) for v in samples))
            samples=array.array('h',(round(32767*math.tanh(2.4*v/peak)/math.tanh(2.4)) for v in samples))
        values=list(samples)+[0]*32
        data=encode2(values,t);blob=banks[bank-125];address=0xc000+len(blob)
        if len(blob)+len(data)>8192:raise ValueError(f'{event}: compressed bank overflow')
        blob.extend(data);pcm_rows.append((bank,address,len(values)))
        pcm_report.append(dict(event=event,source=rid,samples=len(values),bytes=len(data),
                               rate=6991,codec='build14-2bit-low-pair-first',channel=1 if event=='gallop' else 0))
    header='/* Build 14 packed effects: bank, CPU address, exact sample count. */\n'
    header+='static const uint16_t pcm_samples[4][3] __attribute__((section(".ram_bank117.rodata")))={'+','.join('{%d,%d,%d}'%v for v in pcm_rows)+'};\n'
    for i,blob in enumerate(banks):
        header+='const uint8_t pce_pcm_bank%d[] __attribute__((used,retain,section(".ram_bank%d.rodata")))={'%(i,125+i)+','.join(map(str,blob))+'};\n'
    for name,values in [('small_lo',[v&255 for v in t[0]]),('small_hi',[v>>8 for v in t[0]]),
                        ('large_lo',[v&255 for v in t[1]]),('large_hi',[v>>8 for v in t[1]]),
                        ('next_small',t[2]),('next_large',t[3])]:
        header+='const uint8_t pce_adpcm_'+name+'[256] __attribute__((used,retain,section(".ram_bank117.rodata")))={'+','.join(map(str,values))+'};\n'
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
