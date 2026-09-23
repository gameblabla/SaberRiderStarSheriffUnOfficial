#!/usr/bin/env python3
"""Power attack cut-ins for Saber Rider and Fireball (src/power.c) from the special attack clips in the repo root:
the picture up to the first all-white frame (the game's own flash takes over from there) as an MPEG-4 part 2
elementary stream (decoded with the libavcodec already linked for the pack videos), the sound as a PCM16 wav that
runs on ~1.2 s past the cut and fades (the impact rings on over the flash).
  tools/build_power_assets.py  ->  assets/power/{fireball,saber}.m4v + .wav"""
import os, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)); GAME = os.path.dirname(HERE); ROOT = os.path.dirname(GAME)
OUT = os.path.join(GAME, 'assets', 'power')
CLIPS = {   # name: (source, first frame, the first all-white frame, audio end) - frames at 24 fps, found by mean luma
    'fireball': ('Fireball Special Attack [Mrak9b-a3R0].mkv', 1, 278, 12.9),
    'saber':    ('Saber Rider Special Attack [waLWMp-SyhQ].mp4', 2, 234, 11.0),
}
os.makedirs(OUT, exist_ok=True)
for name, (src, f0, f1, aend) in CLIPS.items():
    src = os.path.join(ROOT, src); t0, t1 = f0 / 24.0, (f1 + 1) / 24.0
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-ss', f'{t0:.4f}', '-to', f'{t1:.4f}', '-an',
                    '-c:v', 'mpeg4', '-q:v', '3', '-bf', '0', '-g', '48', '-f', 'm4v', os.path.join(OUT, f'{name}.m4v')], check=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-ss', f'{t0:.4f}', '-to', f'{aend:.4f}', '-vn',
                    '-af', f'afade=t=out:st={aend - t0 - 0.8:.3f}:d=0.8', '-ac', '2', '-ar', '44100', '-c:a', 'pcm_s16le',
                    os.path.join(OUT, f'{name}.wav')], check=True)
    print(name, f'{t1 - t0:.2f} s picture, {aend - t0:.2f} s sound')
