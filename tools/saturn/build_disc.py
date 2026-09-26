#!/usr/bin/env python3
"""Stage the Sega Saturn disc tree (build/saturn/stage) from the original demo packs and our assets.

Makefile.saturn turns the stage into the ISO + CUE (libyaul's make-iso / make-cue; the program goes in as 0.BIN).
Everything sits in the ISO root under an 8.3 upper-case name (src/platform/saturn/cd_sat.c opens files by name):

  PACK.PCK COMMON.PCK LEVELS.PCK MENU.PCK LEVEL1.PCK   the demo's packs, as they are (read block by block)
  TEX.PCK     every graphic as a "SAT1" block (tools/saturn/satbake.py): pack sprites, cblocks and fonts under their
              own id, our PNGs under namehash(path)
  SND.PCK     adp68k ADPCM samples, "ADPK" blocks (core SFX 2/1-bit, asset/voice SFX 4-bit; bake_audio)
  MUSIC.TXT   music id -> CD-DA track (the tracks: build/saturn/tracks, in the CUE)
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
import film  # noqa: E402
import layers  # noqa: E402
import pckwrite  # noqa: E402

pckwrite.CODEC = 'lz40s'   # every compressed block (packs, texture parts, plane chunks) in the SH-2's LZ40 variant
import satbake  # noqa: E402

SPL_XOR, SPC_XOR = 0x53504C00, 0x53504300   # src/platform/saturn/vdp2_planes.c
FRM_XOR = 0x46524D00                          # src/gfx.c: a cblock's frames baked whole (cblock_draw_frame)
# cblocks whose frames the game draws whole, many at a time, baked a second time as one picture per frame: VDP1 then
# draws a frame as a few parts instead of a command per 16x16 tile (the slave's replay of 30-40 tiles a horse was
# most of a stampede frame). Level 1's robot horse (8 x 5 tiles, up to 12 on screen) and the small props / Outrider
# pieces drawn by frame; not the level-1 boss (21 frames of 13 x 7 tiles: ~200 KB for one sprite on screen). The
# briefing's four hero pieces (2DEF1664: 14 x 15 tiles of 32 x 16, all four on screen: 260 tiles a frame, 2.6 fields)
FRAME_BAKED = (0x8873D18C, 0x19BC8FE8, 0x66986CD1, 0x678A6FE0, 0x66956CD4, 0x2DEF1664)
# textures read together, first in tex.pck in the order they are read (one seek, not ~120 ms each): the briefing's
# preload (src/menu.c MS_BRIEFING: its room, the hero pieces, then everything character select shows)
READ_TOGETHER = (0x0EAE8AEB, 0x2DEF1664,
                 0x4813ED48, 0xAE16B01D, 0xE34D3083, 0x5B550481, 0x7673D08E, 0x7255866F, 0x92702CF3, 0x2178AD91,
                 0xC2EBBACE, 0xB48828F4, 0xEC2B5E94, 0x67C9A3D9, 0x3459994C,
                 0x13D53116, 0x699DC4C3, 0x2F75D0AA, 0x74100546, 0xAA051172,
                 0xCDC8A9CC, 0xA9AB3BF0, 0xE14E4D96, 0x72A6B0FB, 0x3F368A4E,
                 0x957325FD, 0xF6CBB2F4, 0xBFFFBB29, 0xEEE2331F, 0x217B03F1, 0x4058897F)
FRAME_ROW_W = 2048   # the frames side by side, rows of at most this many pixels
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


def cblock_frames(px: np.ndarray, meta: bytes) -> tuple[np.ndarray, list[tuple[int, int, int, int]], bytes]:
    """a cblock's frames composed from its tile sheet (gfx.c cblock_draw_frame's layout: tile (col, row) at col * tw,
    row * th), side by side in rows of FRAME_ROW_W; the meta gfx.c reads: u16 frames a row"""
    frames, cols, rows, tw, th, ntiles, sheet_cols, _ = struct.unpack_from('<8H', meta)
    cells = struct.unpack_from(f'<{frames * cols * rows}H', meta, 16)
    fw, fh = cols * tw, rows * th
    per_row = max(1, FRAME_ROW_W // fw)
    img = np.zeros((-(-frames // per_row) * fh, min(frames, per_row) * fw, 4), np.uint8)
    rects = []
    for f in range(frames):
        ox, oy = (f % per_row) * fw, (f // per_row) * fh
        rects.append((ox, oy, fw, fh))
        for r in range(rows):
            for c in range(cols):
                t = cells[(f * rows + r) * cols + c]
                if t == 0xFFFF or t >= ntiles:
                    continue
                sx, sy = (t % sheet_cols) * tw, (t // sheet_cols) * th
                img[oy + r * th:oy + (r + 1) * th, ox + c * tw:ox + (c + 1) * tw] = px[sy:sy + th, sx:sx + tw]
    return img, rects, struct.pack('<HH', per_row, 0)


def bake_textures(data: Path, work: Path, tex: pckwrite.Pack, log, force8: set[int] = frozenset()) -> None:
    """every pack sprite / cblock / font through the game's own gfx.c (tools/dc/texprep.c), then our PNGs"""
    texprep = work / 'texprep'
    subprocess.run(['cc', '-O2', '-std=gnu11', '-Isrc', 'tools/dc/texprep.c', 'src/gfx.c', 'src/font.c', 'src/pack.c',
                    'src/lzo1z.c', 'src/assets.c', 'src/namehash.c', '-o', str(texprep)], cwd=ROOT, check=True)
    dumps = work / 'srgb'
    fresh(dumps)
    ids = subprocess.run([texprep, data, dumps], check=True, capture_output=True, text=True).stdout.split()
    stats = satbake.Stats()
    first = [f'{i:08X}' for i in READ_TOGETHER if f'{i:08X}' in ids]
    for rid in dict.fromkeys(first + ids):   # first appearance: a graphic in two packs is baked once
        kind, px, meta = satbake.load_srgb(dumps / f'{rid}.srgb')
        block = satbake.bake(px, meta, stats, name=rid, kind=kind, force8=int(rid, 16) in force8)
        tex.add(int(rid, 16), 'tex', block)
        log(f'tex {rid} {px.shape[1]}x{px.shape[0]} {len(block) // 1024} KB')
        if kind == 2 and int(rid, 16) in FRAME_BAKED:
            img, rects, fmeta = cblock_frames(px, meta)
            block = satbake.bake(img, fmeta, stats, name=f'{rid} frames', rects=rects, force8=int(rid, 16) in force8)
            tex.add(int(rid, 16) ^ FRM_XOR, 'tex', block)
            log(f'tex {rid} frames: {len(rects)} of {rects[0][2]}x{rects[0][3]}, {len(block) // 1024} KB')
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



def bake_audio(data: Path, work: Path, stage: Path, out: Path, snd: pckwrite.Pack, log) -> None:
    """Saturn game audio (plan 7): nothing decoded or mixed on the SH-2.

    SFX and voices play on celeriyacon's adp68k driver (third_party/scspadpcm): the SCSP DSP decodes up to 8 ADPCM
    channels at 44.1 kHz, fed by the SCSP's own slots. Each sample is the original adpencode's file, looped at its
    start (block 0 has no prediction, so the same data plays once or looped: aud_sat.c sets the loop word), in an
    "ADPK" block: magic, u32le sample count, the .adp bytes. A slot's 16-bit loop registers cap a sample at 8190
    blocks of data bytes: ~2.97 s at 4 bits, 5.9 s at 2, 11.9 s at 1; a longer one drops to fewer bits.
    Music is CD-DA (tracks/Tnn.BIN, 44.1 kHz stereo, a 2 s pregap each), mixed in through the driver's DSP CD input;
    MUSIC.TXT in the ISO root maps a music id to its track.
    """
    aw = work / 'audio'; sfx_dir = aw / 'sfx'; music_dir = aw / 'music'
    aw.mkdir(parents=True, exist_ok=True); sfx_dir.mkdir(exist_ok=True); music_dir.mkdir(exist_ok=True)
    dcprep = work / 'dcprep'
    if not dcprep.exists():
        subprocess.run(['cc', '-O2', '-std=c11', '-Isrc', 'tools/dc/dcprep.c', 'src/pack.c', 'src/lzo1z.c',
                        'src/platform/common/sfx_decode.c', 'src/platform/common/mups.c', '-o', str(dcprep)], cwd=ROOT, check=True)
    if not any(sfx_dir.glob('*.wav')):
        subprocess.run([dcprep, 'sfx', data, sfx_dir], check=True, capture_output=True)
    if not any(music_dir.glob('*')):
        subprocess.run([dcprep, 'music', data, music_dir], check=True, capture_output=True)

    enc = aw / 'adpencode68k'
    src = ROOT / 'third_party/scspadpcm/adpencode.cpp'
    if not enc.exists() or enc.stat().st_mtime < src.stat().st_mtime:
        subprocess.run(['c++', '-O2', '-std=gnu++11', '-fwrapv', '-D_GNU_SOURCE=1', str(src), '-o', str(enc), '-lsndfile'], check=True)

    def encode(key: int, source: Path, fmt: int, tag: str) -> None:
        for f in range(fmt, 3):   # fewer bits when it is too long for the slots' loop registers
            stem = f'{key:08X}-{f}'
            wav, adp = aw / (stem + '.wav'), aw / (stem + '.adp')
            newest = max(source.stat().st_mtime, src.stat().st_mtime)
            if not adp.exists() or adp.stat().st_mtime < newest:
                subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', str(source), '-ac', '1', '-ar', '44100', '-c:a', 'pcm_s16le',
                                '-metadata', 'comment=adp_loop=0', str(wav)], check=True)
                r = subprocess.run([str(enc), str(f), str(wav), str(adp)], capture_output=True, text=True)
                if r.returncode:
                    adp.unlink(missing_ok=True)
                    if 'too large' not in r.stdout:
                        raise ValueError(f'adpencode {source}: {r.stdout.strip()}')
                    continue
                samples = int(subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-count_packets', '-show_entries',
                                              'stream=duration_ts', '-of', 'csv=p=0', str(wav)], capture_output=True, text=True,
                                             check=True).stdout.strip() or 0)
                adp.with_suffix('.n').write_text(str(samples))
                wav.unlink(missing_ok=True)
            samples = int(adp.with_suffix('.n').read_text())
            snd.add(key, 'sample', b'ADPK' + struct.pack('<I', samples) + adp.read_bytes())
            log(f'audio {tag}: {source.name} -> {adp.name} ({adp.stat().st_size} bytes, {samples / 44100:.2f} s)')
            return
        log(f'audio {tag}: {source.name} too long for the driver even at 1 bit, left out')

    # sound RAM holds the 32 core effects for good (aud_keep at boot) and the stage's own samples: the frequently
    # triggered ones at 2 bits, the rest at 1 bit (the codec's 1.5 / 2.5 bits a sample with the block headers)
    core_2bit = {
        0xE418A101,                         # menu/UI tick
        0xC66E1894, 0xC6801BB9,            # player weapon variants
        0xBF4917FF, 0xBF5B14EA, 0xBF6D1599,
        0x89389611, 0x8923950D, 0xFE10EB78,
    }
    for wav in sorted(sfx_dir.glob('*.wav')):
        try: key = int(wav.stem, 16)
        except ValueError: continue
        encode(key, wav, 1 if key in core_2bit else 2, 'pack-sfx')

    for source in sorted((ROOT / 'assets').rglob('*.wav')):
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel) or rel.parts[0] == 'power' and (source.with_suffix('.m4v')).exists():
            continue   # a power clip's voice is in its .CPK
        encode(namehash(rel.as_posix()), source, 0, 'asset-sfx')   # 4-bit: voices/custom effects

    tracks = out / 'tracks'
    for old in tracks.glob('*'):
        old.unlink()
    lines = []
    for source in sorted(p for p in music_dir.iterdir() if p.is_file()):
        try: mid = int(source.stem, 16)
        except ValueError: continue
        track = len(lines) + 2   # track 1 is the data track
        pcm = aw / f'{source.stem}.cdda'
        if not pcm.exists() or pcm.stat().st_mtime < source.stat().st_mtime:
            # 44.1 kHz stereo little-endian, the 2 s pregap first (make-cue: INDEX 00 at 0, INDEX 01 at 2 s), whole sectors
            subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', str(source), '-af', 'adelay=2000|2000', '-ac', '2', '-ar', '44100',
                            '-f', 's16le', str(pcm)], check=True)
            with open(pcm, 'ab') as f:
                f.write(bytes(-pcm.stat().st_size % 2352))
        shutil.copy2(pcm, tracks / f'T{track:02d}.BIN')
        lines.append(f'{mid:08X} {track}')
        log(f'music: {source.name} -> track {track} ({pcm.stat().st_size / 176400:.1f} s)')
    (stage / 'MUSIC.TXT').write_text('\n'.join(lines) + '\n')


def bake_videos(data: Path, work: Path, stage: Path, log) -> None:
    """the demo's intro and briefing videos and our power clips as Sega FILM/CPK files with ADX audio in the ISO root (film.py): the intro fills
    the 224 lines, the briefing is made at the size the briefing screen shows it (a third), the clips at 320x224"""
    dcprep = work / 'dcprep'
    subprocess.run(['cc', '-O2', '-std=c11', '-Isrc', 'tools/dc/dcprep.c', 'src/pack.c', 'src/lzo1z.c',
                    'src/platform/common/sfx_decode.c', 'src/platform/common/mups.c', '-o', str(dcprep)], cwd=ROOT, check=True)
    vids = work / 'video'
    if not (vids / '2FE798C3.m4v').exists():
        vids.mkdir(parents=True, exist_ok=True)
        subprocess.run([dcprep, 'video', data, vids], check=True, capture_output=True)
    xvid = ['-r', '25', '-f', 'm4v']   # the pack videos: raw XviD, 25 fps (tools/dc/build_disc.py)
    sound = lambda p: next((q for q in (p.with_suffix('.ogg'), p.with_suffix('.wav')) if q.exists()), None)
    film.make(vids / 'E46721E5.m4v', stage / 'E46721E5.CPK', work / 'film', (296, 224), '', xvid, sound(vids / 'E46721E5.m4v'), log)
    film.make(vids / '2FE798C3.m4v', stage / '2FE798C3.CPK', work / 'film', (256, 104), '', xvid, sound(vids / '2FE798C3.m4v'), log)
    for clip in sorted((ROOT / 'assets/power').glob('*.m4v')):   # 320x240 at 24 fps, the voice in the .wav beside it
        film.make(clip, stage / (clip.stem.upper()[:8] + '.CPK'), work / 'film', (320, 224), 'crop=320:224:0:8',
                  ['-r', '24', '-f', 'm4v'], clip.with_suffix('.wav'), log)


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
    bake_audio(data, work, stage, out, snd, log)
    bake_videos(data, work, stage, log)
    for source in sorted((ROOT / 'assets').rglob('*')):
        if not source.is_file():
            continue
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        key, ext = namehash(rel.as_posix()), source.suffix.lower()
        if ext == '.m4v':   # an empty entry, so asset_path finds the clip (the video is its .CPK file)
            files.add(key, 'file', file_block(b''))
            continue
        if ext == '.wav':
            continue   # baked into SND.PCK above (a power clip's voice is also in its .CPK)
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
