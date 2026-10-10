#!/usr/bin/env python3
"""Count visible road changes in the supplied Chase H.Q. reference state."""
import argparse,hashlib,json,tempfile
from pathlib import Path
from PIL import Image
from emulator import Emulator,ROOT

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--rom',type=Path,default=ROOT/'PCE/References/Chase H.Q. (USA).pce')
    p.add_argument('--state',type=Path,default=ROOT/'PCE/References/Chase H.Q. (USA).b6098cef5a4729009d446603c650e323-start-of-race.mc0')
    p.add_argument('--out',type=Path,default=Path('artifacts/race-speed'));a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as b,Emulator(a.rom,b,arcade=False) as e:
        e.run(1);e.call('loadstate',str(a.state.resolve()));e.input(1);e.run(240)
        e.screenshot(out/'chase-reference.png');hashes=[]
        for _ in range(120):
            e.run(1);path=Path(b)/'frame.png';e.screenshot(path)
            im=Image.open(path).convert('RGB')
            # Below the buildings and above the player's car: road and its
            # verges, excluding HUD, skyline animation and car animation.
            crop=im.crop((im.width//8,im.height*72//100,im.width*7//8,im.height*80//100))
            hashes.append(hashlib.sha256(crop.tobytes()).hexdigest())
        report=dict(video_frames=120,road_crop_changes=sum(x!=y for x,y in zip(hashes,hashes[1:])),intervals=119)
        (out/'chase-reference.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
if __name__=='__main__':main()
