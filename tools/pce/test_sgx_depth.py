#!/usr/bin/env python3
"""Saloon scenery stays on the rear VDC, even with a stale front-plane hint."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering

def run(out):
    out=out.resolve();r=Rendering(out);elf=out/'app.elf'
    plane=symbol(elf,'pce_sgx_actor_plane')
    with tempfile.TemporaryDirectory(prefix='depth-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,r.address);r.dialogs(e)
        reports=[]
        scene=json.loads((out/'manifest.json').read_text())['scenes'][0]
        for kind,x,y,id in ((13,5792,96,48),(14,7232,144,49),(15,6624,144,50),(17,6544,80,51),(18,7168,80,52)):
            r.position(e,x);r.press(e,8);e.run(90)
            # Real source prop/waypoint and overlapping fighter. Deliberately
            # seed the obsolete VDC0 preference for this scenery actor.
            prop=struct.pack('<4h4B9B',x,y,0,0,0,0,4,4,1,kind,1,0,0,0,0,0,0)
            fighter=struct.pack('<4h4B9B',x,y+16,0,0,0,0,4,4,1,6,20,0,0,0,0,0,0)
            e.write(r.sym['actors'],prop+fighter+bytes(126));e.write(plane,b'\0'+b'\xff'*7)
            r.press(e,8);e.run(30);r.settle(e)
            entries=r.sprite_entries(e,id)
            assert entries,'Scenery fixture must render visible source pieces'
            assert all(vdc==1 for vdc,_index,_row in entries),('Scenery crossed onto foreground VDC',kind,entries)
            fighter_entries=[]
            for fid,sprite in enumerate(scene['sprites']):
                if sprite['name'].startswith('sniper'):
                    fighter_entries+=r.sprite_entries(e,fid)
            fighter_entries=[i for v,i,row in fighter_entries if v==1 and row[0]]
            assert fighter_entries,'Overlapping fighter must remain visible'
            assert min(i for _v,i,_row in entries)>max(fighter_entries),('Scenery SAT entries are in front of fighter',kind)
            e.screenshot(out/f'saloon-depth-{kind}.png')
            reports.append(dict(type=kind,vdc=1,prop_parts=len(entries),fighter_parts=len(fighter_entries)))
        report=dict(props=reports,passed=True)
    (out/'saloon-depth.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));run(p.parse_args().out)
