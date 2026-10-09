#!/usr/bin/env python3
"""Build relocatable LLVM-MOS CD images and verify every archive extent."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from zx02_archive import pack_archives

def main():
    p = argparse.ArgumentParser(); p.add_argument('--out', type=Path, required=True); p.add_argument('--mos', type=Path, required=True)
    args=p.parse_args(); out=args.out.resolve(); mos=args.mos.resolve()
    # Ramrod's arena's code images go into the stage 6 archive's reserved space (build_assets.py 'm6_image'); the archive keeps its size
    m6=out/'m6.bin'
    if m6.exists():
        offset=json.loads((out/'manifest.json').read_text())['scenes'][5]['m6_image']
        archive=bytearray((out/'s6.bin').read_bytes());blob=m6.read_bytes()
        if archive[offset:offset+len(blob)]!=blob:
            archive[offset:offset+len(blob)]=blob;(out/'s6.bin').write_bytes(bytes(archive))
    names=[*[f's{i}.bin' for i in range(1,8)],'ui.bin']
    sgx='-DPCE_SGX' in (out/'build-flags').read_text()
    if sgx:
        report=pack_archives(out,names)
        (out/'compression.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2),flush=True)
    stages=[f's{i}{"_packed" if sgx else ""}.bin' for i in range(1,8)]
    files=['ipl.elf','app.elf',*stages,'font.bin','ui_packed.bin' if sgx else 'ui.bin','victory.bin',*[f'voice{i}.bin' for i in range(4)]]
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
    for number,track in enumerate(audio['tracks'],2):
        if track['track']!=number or track['logical']!=number-2:
            raise RuntimeError('CD-DA source order differs from the driver layout')
        if track['file']!=f'music{number-2:02d}.bin':
            raise RuntimeError('Unexpected CD-DA variant or end-marker track')
        cue.extend([f'FILE "{track["file"]}" BINARY',f'  TRACK {number:02d} AUDIO',
                    '    INDEX 00 00:00:00','    INDEX 01 00:02:00'])
    (out/'saber_rider.cue').write_text('\n'.join(cue)+'\n')

if __name__=='__main__': main()
