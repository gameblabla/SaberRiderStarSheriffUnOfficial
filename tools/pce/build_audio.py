#!/usr/bin/env python3
"""Extract the current game's music and master sector-aligned CD-DA tracks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import array
from adpcm import encode
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
    for i,mid in enumerate(ids):
        pcm=out/f'music{i:02d}.bin';src=music/f'{mid}.ogg'
        if not pcm.exists() or pcm.stat().st_mtime<src.stat().st_mtime:
            subprocess.run(['ffmpeg','-y','-v','error','-i',str(src),'-af','volume=0.9,adelay=2000|2000',
                            '-ac','2','-ar','44100','-f','s16le',str(pcm)],check=True)
            with pcm.open('ab') as f:f.write(bytes(-pcm.stat().st_size%2352))
        tracks.append(dict(id=mid,logical=i,track=i+2,file=pcm.name,sectors=pcm.stat().st_size//2352,
                           sha256=hashlib.sha256(pcm.read_bytes()).hexdigest()))
    # A real next-track endpoint also exists for the final repeatable cue.
    (out/'music_end.bin').write_bytes(bytes(2352*300))
    sfx=work/'sfx';sfx.mkdir(exist_ok=True)
    subprocess.run([str(exe.resolve()),'sfx',str(ROOT/'SaberRider/data'),str(sfx.resolve())],check=True)
    voices=[];rows=[]
    for hero,name in enumerate(('saber','fireball','april','colt')):
        bank=bytearray();samples=[]
        for event,rid in zip(('jump','hurt1','death1'),('EB450AED','8ADE82B6','8ACB81A0')):
            src=sfx/f'{rid}.wav' if hero==1 else ROOT/f'assets/voice/{name}_{event}.wav'
            result=subprocess.run(['ffmpeg','-v','error','-i',str(src),'-ac','1','-ar','8000','-af','volume=0.65','-f','s16le','-'],check=True,capture_output=True)
            pcm=array.array('h');pcm.frombytes(result.stdout)
            data=encode([0]*32+list(pcm)+[0]*64)
            samples.append(dict(event=event,address=len(bank),bytes=len(data),source=str(src.relative_to(ROOT))))
            bank.extend(data)
        if len(bank)>65535:raise ValueError(f'{name}: ADPCM bank exceeds hardware RAM')
        bank.extend(bytes(-len(bank)%2048));(out/f'voice{hero}.bin').write_bytes(bank)
        voices.append(dict(hero=hero,bytes=len(bank),samples=samples))
        rows.append('{'+','.join('{%d,%d}'%(s['address'],s['bytes']) for s in samples)+'}')
    (out/'samples.h').write_text('/* Generated hardware ADPCM bank offsets. */\nstatic const uint16_t voice_samples[4][3][2]={'+','.join(rows)+'};\n')
    (out/'audio.json').write_text(json.dumps(dict(tracks=tracks,end_track=20,voices=voices),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();build(a.out.resolve())
