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
  STAGE.PCK   blocks that replace the demo's, opened before the other packs (tools/saturn/layers.py): a level without
              the tile maps its VDP2 planes draw ("data", the level's id), the planes ("SPL1", id ^ SPL_XOR) and their
              cells (32 KB blocks, id ^ (SPC_XOR + n)), the planes' VDP1 backdrops ("tex")
  SABER.ENV   debug switches (--env), as the Dreamcast's saber.env

The source packs and assets are never modified.
"""
from __future__ import annotations

import argparse
import os
import importlib.util
import re
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
import layers  # noqa: E402
import pckwrite  # noqa: E402
import satbake  # noqa: E402

SPL_XOR, SPC_XOR = 0x53504C00, 0x53504300   # src/platform/saturn/vdp2_planes.c
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


def atlas_rects(rel: str) -> list[tuple[int, int, int, int]] | None:
    """the rectangles the game draws out of one of our atlases (its .txt layout), so each is stored drawable"""
    root = ROOT / 'assets'
    rects: list[tuple[int, int, int, int]] = []
    if rel == 'mode7.png':   # name x y w h frames: frames side by side
        for line in (root / 'mode7.txt').read_text().splitlines():
            f = line.split()
            if len(f) == 6:
                x, y, w, h, n = map(int, f[1:])
                rects += [(x + i * w, y, w, h) for i in range(n)]
    elif rel in ('ramrod/atlas.png', 'space/atlas.png'):   # name frame x y w h ax ay
        for line in (root / rel).with_suffix('.txt').read_text().splitlines():
            f = line.split()
            if len(f) == 8:
                rects.append(tuple(map(int, f[2:6])))
    return rects or None


def levprep(data: Path, work: Path, ids: list[int]) -> None:
    """the given blocks of the demo's packs, as the game reads them, into work/<ID>.levl"""
    exe = work / 'levprep'
    if not exe.exists():
        subprocess.run(['cc', '-O2', '-std=gnu11', '-Isrc', 'tools/saturn/levprep.c', 'src/pack.c', 'src/lzo1z.c', '-o',
                        str(exe)], cwd=ROOT, check=True)
    subprocess.run([exe, data, work, *(f'{i:08X}' for i in ids)], check=True)


def priority_textures(data: Path, work: Path, log) -> set[int]:
    """the graphics of the enemies a planned level spawns under one of its planes (layers.priority_sprites): enemies.c's
    type -> CRHC table, a CRHC's graphics id at +4"""
    src = (ROOT / 'src/enemies.c').read_text()
    table = [int(x, 16) for x in re.findall(r'0x([0-9A-F]{8})', re.search(r'TYPE_CRHC\[28\] = \{(.*?)\};', src, re.S).group(1))]
    fx = [int(x, 16) for x in re.findall(r'0x([0-9A-F]{8})', re.search(r'FX\[\] = \{(.*?)\};', src, re.S).group(1))]
    crhc = lambda t: table[t - 2] if 2 <= t <= 29 else 0xD39700C4 if 30 <= t <= 32 else 0x02A38AFB if t == 1 else 0
    levels = [lid for lid, plan in layers.PLANS.items() if 'dump' not in plan]   # a stage of its own spawns from code
    levprep(data, work, levels + sorted(set(table) | {0xD39700C4, 0x02A38AFB}))
    sprite = lambda c: struct.unpack_from('<I', (work / f'{c:08X}.levl').read_bytes(), 4)[0] if (work / f'{c:08X}.levl').exists() else 0
    ids: set[int] = set()
    for lid in levels:
        got = layers.priority_sprites(work / f'{lid:08X}.levl', crhc, sprite, fx)
        log(f'planes {lid:08X}: palette sprites (drawn under a plane): ' + ' '.join(f'{i:08X}' for i in sorted(got)))
        ids |= got
    return ids


def palette_pngs() -> list[str]:
    """our PNGs drawn under a plane (layers.PLANS palette_pngs: stage 3's sky and moon, Hyperjumper's passes): 8bpp"""
    return [g for plan in layers.PLANS.values() for g in plan.get('palette_pngs', [])]


def bake_textures(data: Path, work: Path, tex: pckwrite.Pack, log, force8: set[int] = frozenset()) -> None:
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
        block = satbake.bake(px, meta, stats, name=rid, kind=kind, force8=int(rid, 16) in force8)
        tex.add(int(rid, 16), 'tex', block)
        log(f'tex {rid} {px.shape[1]}x{px.shape[0]} {len(block) // 1024} KB')
    for source in sorted((ROOT / 'assets').rglob('*.png')):
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        block = satbake.bake(np.array(Image.open(source).convert('RGBA')), b'', stats, name=rel.as_posix(),
                             rects=atlas_rects(rel.as_posix()), force8=any(rel.match(g) for g in palette_pngs()))
        tex.add(namehash(rel.as_posix()), 'tex', block)
        log(f'tex {rel} {len(block) // 1024} KB')
    log(stats.report())


def bake_stages(data: Path, work: Path, stage: pckwrite.Pack, log) -> None:
    """every level with a plane layout (layers.PLANS): its planes, backdrops and slim level block"""
    ids = [f'{i:08X}' for i in layers.PLANS]
    levprep(data, work, [lid for lid, plan in layers.PLANS.items() if 'dump' not in plan] + [0x12DAD1A7])
    stats = satbake.Stats()
    for rid in ids:
        dump = stage_dump(data, work, layers.PLANS[int(rid, 16)].get('dump'))
        res = layers.bake(work / f'{rid}.levl', work / 'srgb', dump)
        for line in res.report:
            log(f'planes {rid}: {line}')
        lid = int(rid, 16)
        if res.slim:
            stage.add(lid, 'data', res.slim, lz4=True)
        stage.add(lid ^ SPL_XOR, 'data', res.spl)
        for k, cells in enumerate(res.spc):
            stage.add(lid ^ (SPC_XOR + k), 'data', cells, lz4=True)
        for tid, _, _, _, img in res.backdrops:
            stage.add(tid, 'tex', satbake.bake(img, b'', stats, name=f'backdrop {tid:08X}', force8=True))
        # a tile bank the planes draw from and VDP1 too: its texture with only VDP1's tiles (the planes have the rest)
        for bank, cells in ({} if dump else layers.vdp1_tiles(work / f'{rid}.levl', res.taken)).items():
            if (bank, 'tex') in stage:
                continue
            kind, px, meta = satbake.load_srgb(work / 'srgb' / f'{bank:08X}.srgb')
            b = layers.levl.load_bank(work / 'srgb' / f'{bank:08X}.srgb')
            tiles = sorted({int(b.cells[(v - 1) % len(b.cells)]) for v in cells} - {0xFFFF})
            rects = [((t % b.sheet_cols) * b.tw, (t // b.sheet_cols) * b.th, b.tw, b.th) for t in tiles]
            block = satbake.bake(px, meta, stats, name=f'{bank:08X} (VDP1 tiles)', kind=kind, rects=rects)
            stage.add(bank, 'tex', block)
            log(f'planes {rid}: tile bank {bank:08X} for VDP1: {len(tiles)} tiles, {len(block) // 1024} KB')
        if not preview_dir:
            continue
        from PIL import Image
        Image.fromarray(layers.preview(res, work / f'{rid}.levl', work / 'srgb', [0, 1500, 3000, 5000, 6000], dump=dump)).save(
            preview_dir / f'planes_{rid}.png')


def stage_dump(data: Path, work: Path, stage: int | None) -> Path | None:
    """a stage's tile layers as the game builds them (stages 3-5: level.c level_dump_layers, on the headless build)"""
    if stage is None:
        return None
    exe = ROOT / 'build/headless/saber_headless'
    subprocess.run(['make', '-f', 'Makefile.headless', '-j8'], cwd=ROOT, check=True, capture_output=True)
    out = work / f'stage{stage}.layers'
    env = dict(os.environ, SABER_ASSETS=str(ROOT / 'assets'), SABER_FRAMES='1', SABER_DUMPLAYERS=str(out))
    subprocess.run([exe, data, str(stage)], cwd=ROOT, env=env, check=True, capture_output=True)
    return out


preview_dir: Path | None = None


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

    tex, snd, files, stage_pack = pckwrite.Pack(), pckwrite.Pack(), pckwrite.Pack(), pckwrite.Pack()
    bake_textures(data, work, tex, log, priority_textures(data, work, log))
    global preview_dir
    preview_dir = out
    bake_stages(data, work, stage_pack, log)
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
    for name, pack in (('TEX.PCK', tex), ('SND.PCK', snd), ('FILES.PCK', files), ('STAGE.PCK', stage_pack)):
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
