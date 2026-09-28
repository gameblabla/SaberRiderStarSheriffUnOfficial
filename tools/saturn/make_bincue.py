#!/usr/bin/env python3
"""Turns build/saturn/saber_rider.iso (2048 B/sector, from libyaul's xorrisofs via build.post.iso-cue.mk) plus
the baked CD-DA tracks (build/saturn/tracks/T02.BIN.. from build_disc.py's bake_audio) into one saber_rider.bin
(MODE1/2352 raw data track + the CD-DA tracks, back to back) and a single-FILE saber_rider.cue - the way a real
Saturn disc looks (see e.g. a redump .cue: one FILE, every track just an INDEX offset into it), replacing
libyaul's make-cue output (one FILE per CD-DA track, referencing build/saturn/tracks/T*.BIN by relative path).

Run as the last step of `make -f Makefile.saturn disc` (see Makefile.saturn); overwrites saber_rider.cue.

Usage: make_bincue.py OUT_DIR
  OUT_DIR/saber_rider.iso and OUT_DIR/tracks/T*.BIN must already exist (built by Makefile.saturn / build_disc.py).
Writes OUT_DIR/saber_rider.bin and OUT_DIR/saber_rider.cue.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHUNK = 1 << 20


def frames_to_msf(frames: int) -> str:
    m, rem = divmod(frames, 75 * 60)
    s, f = divmod(rem, 75)
    return f'{m:02d}:{s:02d}:{f:02d}'


def append_file(dst, src: Path) -> int:
    size = 0
    with open(src, 'rb') as f:
        while chunk := f.read(CHUNK):
            dst.write(chunk)
            size += len(chunk)
    return size


def main() -> None:
    out = Path(sys.argv[1])
    iso = out / 'saber_rider.iso'
    tracks_dir = out / 'tracks'
    bin_path = out / 'saber_rider.bin'
    cue_path = out / 'saber_rider.cue'
    work = out / 'work'
    work.mkdir(parents=True, exist_ok=True)

    tool = work / 'iso2352'
    src = HERE / 'iso2352.c'
    if not tool.exists() or tool.stat().st_mtime < src.stat().st_mtime:
        subprocess.run(['cc', '-O2', '-o', str(tool), str(src)], check=True)

    track01 = work / 'track01.bin'
    subprocess.run([str(tool), str(iso), str(track01)], check=True)

    audio_tracks = sorted(tracks_dir.glob('T*.BIN'))
    if not audio_tracks:
        raise SystemExit(f'no CD-DA tracks found in {tracks_dir}')

    cue_lines = [f'FILE "{bin_path.name}" BINARY', '  TRACK 01 MODE1/2352', '    INDEX 01 00:00:00']
    with open(bin_path, 'wb') as f:
        frame = append_file(f, track01) // 2352
        for i, t in enumerate(audio_tracks, start=2):
            size = t.stat().st_size
            if size % 2352:
                raise ValueError(f'{t} is not a whole number of 2352-byte sectors')
            append_file(f, t)
            # every track file already starts with a 2 s (150-frame) pregap (build_disc.py bake_audio):
            # INDEX 00 marks that pregap's start, INDEX 01 the audio 2 s later, both within the same FILE
            cue_lines.append(f'  TRACK {i:02d} AUDIO')
            cue_lines.append(f'    INDEX 00 {frames_to_msf(frame)}')
            cue_lines.append(f'    INDEX 01 {frames_to_msf(frame + 150)}')
            frame += size // 2352

    cue_path.write_text('\n'.join(cue_lines) + '\n')
    track01.unlink()
    print(f'{bin_path} ({bin_path.stat().st_size} bytes), {cue_path} ({len(audio_tracks)} audio tracks)')


if __name__ == '__main__':
    main()
