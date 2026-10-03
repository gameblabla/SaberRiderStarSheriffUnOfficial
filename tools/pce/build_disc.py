#!/usr/bin/env python3
"""Build relocatable LLVM-MOS CD images and verify every archive extent."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def main():
    p = argparse.ArgumentParser(); p.add_argument('--out', type=Path, required=True); p.add_argument('--mos', type=Path, required=True)
    args=p.parse_args(); out=args.out.resolve(); mos=args.mos.resolve()
    files=['ipl.elf','app.elf',*[f's{i}.bin' for i in range(1,8)],'font.bin','ui.bin',*[f'voice{i}.bin' for i in range(4)]]
    result=subprocess.run([str(mos/'bin/pce-mkcd'),'--ipl','ipl.bin','saber_rider.iso',*files],cwd=out,check=True,capture_output=True,text=True)
    print(result.stderr)
    extents=[]; image=(out/'saber_rider.iso').read_bytes()
    for name,sector,count in re.findall(r'Writing "([^"]+)".*?sector (\d+), size (\d+)',result.stderr):
        sector,count=int(sector),int(count)
        row=dict(file=name,sector=sector,sectors=count)
        if name.endswith('.bin'):
            data=(out/name).read_bytes()
            if image[sector*2048:sector*2048+len(data)] != data: raise RuntimeError(f'Disc extent mismatch: {name}')
            row['sha256']=hashlib.sha256(data).hexdigest()
        extents.append(row)
    if len([e for e in extents if e['file'].startswith('s') and e['file'].endswith('.bin')]) != 7: raise RuntimeError('Missing stage extents')
    audio=json.loads((out/'audio.json').read_text())
    (out/'disc.json').write_text(json.dumps(dict(extents=extents,data_sectors=len(image)//2048,audio=audio),indent=2)+'\n')
    cue=['FILE "saber_rider.iso" BINARY','  TRACK 01 MODE1/2048','    INDEX 01 00:00:00']
    for track in sorted(audio['tracks'],key=lambda t:t['track']):
        if track['id']=='END':
            cue.extend([f'FILE "{track["file"]}" BINARY',f'  TRACK {track["track"]:02d} AUDIO','    INDEX 01 00:00:00'])
        else:
            cue.extend([f'FILE "{track["file"]}" BINARY',f'  TRACK {track["track"]:02d} AUDIO',
                        '    INDEX 00 00:00:00','    INDEX 01 00:02:00'])
    (out/'saber_rider.cue').write_text('\n'.join(cue)+'\n')

if __name__=='__main__': main()
