#!/usr/bin/env python3
"""Re-encode every staged DCMV video with the requested compression backend.

Only the videos are touched (the staged packs/music are left alone), so this is the
cheap way to move a built disc from LZ4 to LZ40 (or back). DCMV videos are decoded from
the demo's own video.pck, converted to PVR VQ textures and repacked, exactly as
build_disc.convert_video() does at build time.

usage: reencode_videos.py [lz40|lz4]   (default lz40)
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/dc'))
from build_disc import convert_video  # noqa: E402

STAGE = ROOT / 'build/dc/stage'
EXTRACTED = ROOT / 'build/dc/work/extracted/video'
# the .m4v source and its soundtrack, per staged video id
SOURCES = {
    '2FE798C3': (25, '2FE798C3.m4v', '2FE798C3.wav'),
    '3EC59DE4': (25, '3EC59DE4.m4v', '3EC59DE4.wav'),
    'E46721E5': (25, 'E46721E5.m4v', 'E46721E5.ogg'),
    'fireball': (24, None, None),          # assets/power, no soundtrack
    'saber': (24, None, None),
}


def ctype_of(path: Path) -> int:
    return path.open('rb').read(50)[45]


def main() -> int:
    backend = sys.argv[1] if len(sys.argv) > 1 else 'lz40'
    if backend not in ('lz40', 'lz4'):
        print('backend must be lz40 or lz4')
        return 2

    jobs: list[tuple[Path, int, Path | None, Path | None]] = []
    for target in sorted((STAGE / 'video').glob('*.dcmv')) + sorted((STAGE / 'assets/power').glob('*.dcmv')):
        stem = target.stem
        fps, src, snd = SOURCES.get(stem, (25, f'{stem}.m4v', None))
        source = EXTRACTED / src if src else ROOT / 'assets/power' / f'{stem}.m4v'
        soundtrack = EXTRACTED / snd if snd else None
        if not source.is_file():
            print(f'skip {stem}: no source at {source}')
            continue
        if soundtrack and not soundtrack.is_file():
            soundtrack = None
        jobs.append((target, fps, source, soundtrack))

    for target, fps, source, soundtrack in jobs:
        before = ctype_of(target)
        print(f'\n=== {target.relative_to(STAGE)} (ctype {before} -> {backend})', flush=True)
        convert_video(source, soundtrack, target, fps,
                      ROOT / f'build/dc/work/fmv_reencode_{backend}', compression=backend)
        after = ctype_of(target)
        nu, nt, fsz, mx, ao = struct.unpack('<IIIII', target.open('rb').read(45)[25:45])
        print(f'    ctype={after} frames={nu} frame_size={fsz} max_compressed={mx} '
              f'size={target.stat().st_size}', flush=True)
        if after != (2 if backend == 'lz40' else 0):
            print(f'    ERROR: expected ctype {2 if backend == "lz40" else 0}, got {after}')
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
