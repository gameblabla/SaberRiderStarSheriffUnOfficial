#!/usr/bin/env python3
"""Stage the Sega Saturn disc tree (build/saturn/stage) from the original demo packs and our assets.

Makefile.saturn turns the stage into the ISO + CUE (libyaul's make-iso / make-cue; the program goes in as A.BIN).
Everything sits in the ISO root under an 8.3 upper-case name (src/platform/saturn/cd_sat.c opens files by name):

  PACK.PCK COMMON.PCK LEVELS.PCK MENU.PCK LEVEL1.PCK   the demo's packs, as they are (read block by block)
  TEX.PCK     every graphic as a "SAT1" block (tools/saturn/satbake.py): pack sprites, cblocks and fonts under their
              own id, our PNGs under namehash(path)
  SND.PCK     the samples (the sound driver milestone fills it; empty until then)
  FILES.PCK   our other files (text, level blobs) and the RGBA of the few images the game reads as pixels, stored as
              host-order (big-endian) 0xAABBGGRR integers, the way the core reads pixels
  SABER.ENV   debug switches (--env), as the Dreamcast's saber.env

The source packs and assets are never modified.
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools/dc'))
sys.path.insert(0, str(HERE))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
import pckwrite  # noqa: E402
import satbake  # noqa: E402
_spec = importlib.util.spec_from_file_location('dc_build_disc', ROOT / 'tools/dc/build_disc.py')
dc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dc)   # the Dreamcast builder: its asset tables and helpers
IMAGES, PACKS, namehash, unused, file_block = dc.IMAGES, dc.PACKS, dc.namehash, dc.unused, dc.file_block


def run(*args: object, cwd: Path | None = None) -> subprocess.CompletedProcess:
    cmd = [str(a) for a in args]
    print('+', ' '.join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True)


def fresh(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def image_block(path: Path, crop) -> bytes:
    """src/assets.c png_load_rgba: "RGBA", u16 w, h, the stored rectangle's u16 x, y, w, h (w 0: all), 16 bytes 0; the
    header little-endian as the core reads it, the pixels as the SH-2's own uint32 0xAABBGGRR (bytes A, B, G, R)"""
    px = np.array(Image.open(path).convert('RGBA'))
    x, y, w, h = crop(path) if crop else (0, 0, 0, 0)
    part = px[y:y + h, x:x + w] if crop else px
    return b'RGBA' + struct.pack('<6H16x', px.shape[1], px.shape[0], x, y, w, h) + part[..., ::-1].tobytes()


def bake_textures(data: Path, work: Path, tex: pckwrite.Pack, log) -> None:
    """every pack sprite / cblock / font through the game's own gfx.c (tools/dc/texprep.c), then our PNGs"""
    texprep = work / 'texprep'
    subprocess.run(['cc', '-O2', '-std=gnu11', '-Isrc', 'tools/dc/texprep.c', 'src/gfx.c', 'src/font.c', 'src/pack.c',
                    'src/lzo1z.c', 'src/assets.c', 'src/namehash.c', '-o', str(texprep)], cwd=ROOT, check=True)
    dumps = work / 'srgb'
    fresh(dumps)
    ids = subprocess.run([texprep, data, dumps], check=True, capture_output=True, text=True).stdout.split()
    stats = satbake.Stats()
    for rid in dict.fromkeys(ids):   # first appearance: a graphic in two packs is baked once
        kind, px, meta = satbake.load_srgb(dumps / f'{rid}.srgb')
        block = satbake.bake(px, meta, stats, name=rid)
        tex.add(int(rid, 16), 'tex', block)
        log(f'tex {rid} {px.shape[1]}x{px.shape[0]} {len(block) // 1024} KB')
    for source in sorted((ROOT / 'assets').rglob('*.png')):
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        block = satbake.bake(np.array(Image.open(source).convert('RGBA')), b'', stats, name=rel.as_posix())
        tex.add(namehash(rel.as_posix()), 'tex', block, lz4=True)
        log(f'tex {rel} {len(block) // 1024} KB')
    log(stats.report())


def build(args: argparse.Namespace) -> None:
    out = args.out.resolve()
    stage, work = out / 'stage', out / 'work'
    data = args.data.resolve()
    for name in PACKS:
        if not (data / name).is_file():
            raise FileNotFoundError(data / name)
    fresh(stage)
    work.mkdir(parents=True, exist_ok=True)
    (out / 'tracks').mkdir(exist_ok=True)
    for name in PACKS:
        shutil.copy2(data / name, stage / name.upper())
    logf = open(out / 'bake.log', 'w')

    def log(msg: str) -> None:
        print(msg, file=logf, flush=True)

    tex, snd, files = pckwrite.Pack(), pckwrite.Pack(), pckwrite.Pack()
    bake_textures(data, work, tex, log)
    for source in sorted((ROOT / 'assets').rglob('*')):
        if not source.is_file():
            continue
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        key, ext = namehash(rel.as_posix()), source.suffix.lower()
        if ext in ('.wav', '.m4v'):
            continue   # sound driver / FMV milestones
        if ext == '.png':
            if rel.as_posix() in IMAGES:
                files.add(key, 'image', image_block(source, IMAGES[rel.as_posix()]), lz4=True)
        else:
            files.add(key, 'file', file_block(source.read_bytes()), lz4=True)
    for name, pack in (('TEX.PCK', tex), ('SND.PCK', snd), ('FILES.PCK', files)):
        size = pack.write(stage / name)
        print(f'{name}: {len(pack.blocks)} blocks, {size / 1024 / 1024:.1f} MiB', flush=True)
    logf.close()
    if args.env:
        (stage / 'SABER.ENV').write_text(Path(args.env).read_text() if Path(args.env).is_file() else args.env.replace(';', '\n') + '\n')
    for txt in ('ABS.TXT', 'BIB.TXT', 'CPY.TXT'):
        (stage / txt).write_text('Saber Rider and the Star Sheriffs - fan reconstruction of the 2017 demo\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data', type=Path, required=True, help='original demo data directory')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--env', help='SABER.ENV: a file, or NAME=value;NAME=value')
    args = parser.parse_args()
    try:
        build(args)
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
        print(f'disc build failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
