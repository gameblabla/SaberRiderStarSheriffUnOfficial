#!/usr/bin/env python3
"""Rebuild clean SGX moon pages at the reported save-state camera position."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering


def verify(out):
    out=out.resolve();c=Rendering(out);checks=[]
    with tempfile.TemporaryDirectory(prefix='moon-restore-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=True) as e:
        boot(e,c.address);c.stage(e,3)
        page=symbol(out/'app.elf','pce_sgx_sky_page')
        # The reported state has camera 4074 and a stray decorated sky cell
        # just right of the moon. Copying the displayed BAT perpetuates it.
        c.position(e,4194);c.settle(e);c.sky(e)
        current=e.memory(page,1)[0]
        e.write((current*0x800+7*64+51)*2,struct.pack('<H',0xe300),'vram1')
        c.position(e,4200);c.settle(e);c.sky(e)
        e.screenshot(out/'moon-reported-camera-fixed.png')
        for camera in (4074,4077,4080,4083,4086,4089,4092,4095,
                       4092,4089,4086,4083,4080,4077,4074,5000,5200,5000,4074):
            c.position(e,camera+120);c.settle(e)
            cells=c.sky(e)
            checks.append(dict(camera=c.metrics(e)['camera_x'],page=e.memory(page,1)[0],cells=cells))
        assert {check['page'] for check in checks}=={0,1}
        (out/'moon-restore-verification.json').write_text(json.dumps(checks,indent=2)+'\n')
        print('Native moon restoration passed',len(checks),'camera checks',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'));verify(p.parse_args().out)
