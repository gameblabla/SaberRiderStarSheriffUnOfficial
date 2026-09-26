#!/usr/bin/env python3
"""Re-encode the intro video with a chosen DCMV compression backend, repack the disc,
and capture a flycast automation screenshot. Usage: flycast_video_test.py <lz4|lz40> [replay_seconds]"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path('/home/anonymous/Documents/DEV/Saber_Rider/game')
STAGE = ROOT / 'build/dc/stage'
WORK = ROOT / 'build/dc/work'
FLY = Path('/home/anonymous/Documents/DEV/Saber_Rider/Dreamcast/flycast-bin')
SH4_MAIN = 200_000_000


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    backend = sys.argv[1] if len(sys.argv) > 1 else 'lz4'
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
    tmpdir = Path('/tmp/flyauto')
    (tmpdir / 'scripts').mkdir(parents=True, exist_ok=True)
    # the staged tree (assets) is shared; only the boot image is rebuilt, into build/dc-test
    if (ROOT / 'build/dc-test/stage').is_dir():
        shutil.rmtree(ROOT / 'build/dc-test/stage')
    (ROOT / 'build/dc-test').mkdir(parents=True, exist_ok=True)
    shutil.copytree(STAGE, ROOT / 'build/dc-test/stage', symlinks=True)

    # replay: no-op events every 10ms of SH4 time until `secs`, then EOF -> screenshot
    end = int(secs * SH4_MAIN)
    with open(tmpdir / 'scripts' / 'saber_rider.input', 'w') as f:
        t = 0
        while t <= end:
            f.write(f'{t} button 0 ffff\n')
            t += 10_000_000

    # re-encode intro video with the requested backend
    video = STAGE / 'video' / 'E46721E5.dcmv'
    backup = STAGE / 'video' / f'E46721E5.{backend}.bak'
    if not backup.exists() and video.exists():
        shutil.copy2(video, backup)
    print('re-encoding intro with', backend, flush=True)
    env = dict(os.environ)
    sys.path.insert(0, str(ROOT / 'tools/dc'))
    from build_disc import convert_video
    r = ROOT
    convert_video(r / 'build/dc/work/extracted/video/E46721E5.m4v',
                  r / 'build/dc/work/extracted/video/E46721E5.ogg',
                  video, 25, r / f'build/dc/work/fmv_test_{backend}', compression=backend)

    ctype = video.open('rb').read(50)[45]
    print('video ctype =', ctype, 'size =', video.stat().st_size, flush=True)

    # repack disc (reuse stage tree)
    run(['python3', 'tools/dc/build_disc.py', '--elf', 'saber_rider.elf',
         '--out', 'build/dc-test', '--repack', '--pad-to', '0'], cwd=ROOT)

    # run flycast automation (GL) and grab its framebuffer screenshot
    for p in tmpdir.glob('screenshot.png'):
        p.unlink()
    subprocess.run(['pkill', '-f', 'Xvfb :97'], capture_output=True)
    subprocess.Popen(['Xvfb', ':97', '-screen', '0', '640x480x24'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    shot = tmpdir / 'screenshot.png'
    e2 = dict(os.environ, DISPLAY=':97', SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
              LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe')
    proc = subprocess.Popen(
        [str(FLY / 'flycast-automation'),
         '-config', f'config:UseReios=yes,record:replay_input=yes,pvr:rend=0',
         str(ROOT / 'build/dc-test/saber_rider.cdi')],
        cwd=tmpdir, env=e2, stdout=open('/tmp/flycast_test.log', 'w'),
        stderr=subprocess.STDOUT)
    for _ in range(120):
        if shot.exists() or proc.poll() is not None:
            break
        time.sleep(1)
    time.sleep(1)
    if not proc.poll() is None if False else proc.poll() is None:
        proc.terminate()
    subprocess.run(['pkill', '-f', 'Xvfb :97'], capture_output=True)
    if shot.exists():
        dest = Path(f'/tmp/shot_{backend}.png')
        shutil.copy2(shot, dest)
        print('screenshot:', dest, dest.stat().st_size, 'bytes', flush=True)
    else:
        print('NO SCREENSHOT (proc rc =', proc.poll(), ')', flush=True)
        print(open('/tmp/flycast_test.log').read()[-1500:])


if __name__ == '__main__':
    main()
