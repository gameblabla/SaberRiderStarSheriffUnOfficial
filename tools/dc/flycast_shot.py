#!/usr/bin/env python3
"""Run a given ELF + staged tree through flycast-automation and save its framebuffer screenshot.

usage: flycast_shot.py <elf> <cdi> <out.png> [replay_seconds]"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

SH4_MAIN = 200_000_000
FLY = Path('/home/anonymous/Documents/DEV/Saber_Rider/Dreamcast/flycast-bin')
TMP = Path('/tmp/flyauto')


def main():
    elf, cdi, out = sys.argv[1], sys.argv[2], sys.argv[3]
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 15.0
    TMP.mkdir(parents=True, exist_ok=True)
    (TMP / 'scripts').mkdir(parents=True, exist_ok=True)
    end = int(secs * SH4_MAIN)
    with open(TMP / 'scripts' / 'saber_rider.input', 'w') as f:
        t = 0
        while t <= end:
            f.write(f'{t} button 0 ffff\n')
            t += 10_000_000
    shot = TMP / 'screenshot.png'
    if shot.exists():
        shot.unlink()
    subprocess.run(['pkill', '-f', 'Xvfb :97'], capture_output=True)
    subprocess.Popen(['Xvfb', ':97', '-screen', '0', '640x480x24'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    env = dict(os.environ, DISPLAY=':97', SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
               LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe')
    proc = subprocess.Popen(
        [str(FLY / 'flycast-automation'),
         '-config', 'config:UseReios=yes,record:replay_input=yes,pvr:rend=0', cdi],
        cwd=TMP, env=env, stdout=open('/tmp/flycast_shot.log', 'w'), stderr=subprocess.STDOUT)
    for _ in range(180):
        if shot.exists() or proc.poll() is not None:
            break
        time.sleep(1)
    time.sleep(1)
    if proc.poll() is None:
        proc.terminate()
    subprocess.run(['pkill', '-f', 'Xvfb :97'], capture_output=True)
    if shot.exists():
        shutil.copy2(shot, out)
        print('screenshot:', out, Path(out).stat().st_size, 'bytes')
    else:
        print('NO SCREENSHOT rc=', proc.poll())
        print(open('/tmp/flycast_shot.log').read()[-1200:])


if __name__ == '__main__':
    main()
