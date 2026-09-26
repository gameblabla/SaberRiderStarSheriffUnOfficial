#!/usr/bin/env python3
"""Run a given CDI through flycast-automation with a pre-written replay file and save the
framebuffer screenshot. usage: flycast_shot2.py <cdi> <out.png> [display_num]"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

FLY = Path('/home/anonymous/Documents/DEV/Saber_Rider/Dreamcast/flycast-bin')
TMP = Path('/tmp/flyauto')


def main():
    cdi, out = sys.argv[1], sys.argv[2]
    disp = sys.argv[3] if len(sys.argv) > 3 else '97'
    shot = TMP / 'screenshot.png'
    if shot.exists():
        shot.unlink()
    subprocess.run(['pkill', '-f', f'Xvfb :{disp}'], capture_output=True)
    subprocess.Popen(['Xvfb', f':{disp}', '-screen', '0', '640x480x24'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    env = dict(os.environ, DISPLAY=f':{disp}', SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
               LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe')
    proc = subprocess.Popen(
        [str(FLY / 'flycast-automation'),
         '-config', 'config:UseReios=yes,record:replay_input=yes,pvr:rend=0', cdi],
        cwd=TMP, env=env, stdout=open('/tmp/flycast_shot.log', 'w'), stderr=subprocess.STDOUT)
    for _ in range(300):
        if shot.exists() or proc.poll() is not None:
            break
        time.sleep(1)
    time.sleep(1)
    if proc.poll() is None:
        proc.terminate()
    subprocess.run(['pkill', '-f', f'Xvfb :{disp}'], capture_output=True)
    if shot.exists():
        shutil.copy2(shot, out)
        print('screenshot:', out, Path(out).stat().st_size, 'bytes')
    else:
        print('NO SCREENSHOT rc=', proc.poll())
        print(open('/tmp/flycast_shot.log').read()[-1200:])


if __name__ == '__main__':
    main()
