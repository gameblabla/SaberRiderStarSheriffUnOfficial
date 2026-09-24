#!/usr/bin/env python3
"""Build a self-boot Dreamcast CDI from the KOS ELF and original demo packs.

The original E2DM packs are user supplied. All generated media stays under
--out; the source packs and assets are never modified.

Nothing on the disc is decoded at run time: every texture (the packs' sprites,
tile banks and fonts, our PNGs) is baked into data/tex.pck in the PVR's own
formats (tools/dc/texbake.py), every sample (the packs' sfx, our WAVs) into
data/snd.pck as AICA ADPCM, and our other files (text, level blobs, the RGBA of
the few images the game reads pixels from) into data/files.pck. Each block is
32-byte aligned and padded, read in one go and DMA'd on. Music (ADX) and video
(DCMV) stay files: they stream.

A CD-R on the Dreamcast is read at constant linear velocity from the inside
out; the image is padded (a dummy file sorted first) so the game's data sits at
the outer part of the disc, away from the slow, seek-heavy inner tracks.
"""
from __future__ import annotations

import argparse
import datetime
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
import pckwrite  # noqa: E402
import texbake  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
KOS = Path('/opt/toolchains/dc/kos')
FMV = ROOT / 'third_party/dreamcast-fmv'
PACKS = ('pack.pck', 'common.pck', 'levels.pck', 'menu.pck', 'level1.pck')
# assets/ that the game never loads (sources of the ones it does, retired versions)
UNUSED = ('old_april.png', 'stage2_victory.png', 'stage2_victory_og.png', 'mode7_alt.png', 'april_victory_stage1.png',
          'colt_victory_stage1.png', 'fireball_victory_stage1.png', 'saber_victory_stage1.png',
          'stage3_night_sky.png', 'stage3_red_moon.png', 'stage3_static_*.png', 'stage3/native/stage3_static_*.png',
          'stage3/native/manifest.json')
# images whose pixels the game reads (floor materials, hit masks): their RGBA goes to files.pck besides the texture
IMAGES = ('mode7.png', 'ramrod/floor.png', 'space/boss.png')
PAD_TO_MIB = 650   # the padded image size: an 80-minute CD-R holds ~700 MiB, less the second session's lead-in/out


def namehash(name: str) -> int:
    """src/namehash.c: the E2DM resource id of a name (our assets' ids in the baked packs)"""
    b = name.encode()
    n = len(b)
    sc = lambda v: v - 256 if v > 127 else v
    a = sum(b[i] * (i + 1) for i in range(n)) & 0xFFFFFFFF
    if n == 1:
        c = sc(b[0]) & 0xFFFFFFFF
        return (((c | (c << 8)) << 16) | a) & 0xFFFFFFFF
    bb = sum(b[i + 1] * (i + 1) for i in range(n - 1)) & 0xFFFFFFFF
    h = ((bb << 16) | a) & 0xFFFFFFFF
    for i in range(n - 1):
        h = (h + (~(((sc(b[i]) << 24) | (sc(b[i + 1]) & 0xFFFFFFFF)) & 0xFFFFFFFF))) & 0xFFFFFFFF
    c = sc(b[n - 1]); j = n - 1
    while True:
        u = c & 0xFFFFFFFF
        j -= 1; c = sc(b[j])
        u = ((c >> 8) & 0xFFFFFFFF) | u
        h ^= ((u & 0xFF) | ((~u) << 8)) & 0xFFFFFFFF
        if j == 0:
            break
    return h & 0xFFFFFFFF


def run(*args: object, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    cmd = [str(a) for a in args]
    print('+', ' '.join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def need(path: Path) -> Path:
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def fresh(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def sample_block(source: Path, work: Path) -> bytes:
    """a snd.pck block: KOS wav2adpcm's mono 4-bit ADPCM at the source rate, padded to whole 32-byte units with
    0x80 bytes (+step/8, -step/8: holds the level), behind "SMPL", rate, samples, bytes"""
    pcm, adpcm = work / 'sample.pcm.wav', work / 'sample.adpcm.wav'
    subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
                    '-i', source, '-ac', '1', '-c:a', 'pcm_s16le', pcm], check=True)
    subprocess.run([need(KOS / 'utils/wav2adpcm/wav2adpcm'), '-t', pcm, adpcm], check=True, capture_output=True)
    d = adpcm.read_bytes()
    pos, rate, data = 12, 0, None
    while pos + 8 <= len(d):
        tag, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
        if tag == b'fmt ':
            rate = struct.unpack_from('<I', d, pos + 12)[0]
        elif tag == b'data':
            data = d[pos + 8:pos + 8 + size]
            break
        pos += 8 + size + (size & 1)
    if data is None or not rate:
        raise ValueError(f'{source}: wav2adpcm output unreadable')
    data += b'\x80' * (-len(data) % 32)
    return b'SMPL' + struct.pack('<III16x', rate, len(data) * 2, len(data)) + data


def file_block(data: bytes) -> bytes:
    return b'FILE' + struct.pack('<I24x', len(data)) + data


def image_block(path: Path) -> bytes:
    px = np.array(Image.open(path).convert('RGBA'))
    return b'RGBA' + struct.pack('<HH24x', px.shape[1], px.shape[0]) + px.tobytes()


def unused(rel: Path) -> bool:
    return any(rel.match(p) and len(rel.parts) == len(Path(p).parts) for p in UNUSED)


def bake_textures(data: Path, work: Path, tex: pckwrite.Pack, log) -> None:
    """every pack sprite / cblock / font through the game's own gfx.c (texprep), then our PNGs, into tex.pck"""
    texprep = work / 'texprep'
    run('cc', '-O2', '-std=gnu11', '-Isrc', 'tools/dc/texprep.c', 'src/gfx.c', 'src/font.c', 'src/pack.c',
        'src/lzo1z.c', 'src/assets.c', 'src/namehash.c', '-o', texprep, cwd=ROOT)
    dumps = work / 'srgb'
    fresh(dumps)
    ids = subprocess.run([texprep, data, dumps], check=True, capture_output=True, text=True).stdout.split()
    total = 0
    for rid in dict.fromkeys(ids):   # first appearance: a graphic in two packs is baked once
        kind, px, meta = texbake.load_srgb(dumps / f'{rid}.srgb')
        if kind == 2:
            px, meta = texbake.relayout_cblock(px, meta)
        block, desc, vram = texbake.bake(px, meta)
        tex.add(int(rid, 16), 'tex', block)
        total += vram
        log(f'tex {rid} {px.shape[1]}x{px.shape[0]} {desc} {vram // 1024} KB')
    for source in sorted((ROOT / 'assets').rglob('*.png')):
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        block, desc, vram = texbake.bake(np.array(Image.open(source).convert('RGBA')),
                                         log=lambda m, r=rel: log(f'    {r}: {m}'))
        tex.add(namehash(rel.as_posix()), 'tex', block)
        total += vram
        log(f'tex {rel} {desc} {vram // 1024} KB')
    log(f'textures: {total // 1024} KB of VRAM if all were loaded at once')


def convert_video(source: Path, soundtrack: Path | None, target: Path,
                  fps: int, work: Path) -> None:
    """Use the project's DCMV v6 packer, with 512x256 VQ YUV textures."""
    fresh(work)
    target.parent.mkdir(parents=True, exist_ok=True)
    # The raw XviD elementary streams can be misdetected as GSM by ffmpeg.
    # Put them in an MP4 container without re-encoding and assign the known
    # source frame rate before the converter extracts individual frames.
    container = work / 'input.mp4'
    run('ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
        '-r', str(fps), '-f', 'm4v', '-i', source, '-c:v', 'copy', container)
    env = os.environ.copy()
    env.update({
        'INPUT': str(container),
        'AUDIOINPUT': str(soundtrack or source),
        'FINAL_OUTPUT': str(target),
        'OUTPUT_DIR': str(work / 'output'),
        'UNIQUE_FRAMES': str(work / 'unique'),
        'TEMP_DIR': str(work / 'frames'),
        'FPS': str(fps),
        'FORMAT': 'yuv422',
        'USE_STRIDED': 'false',
        'SCALE_WIDTH': '320',
        'SCALE_HEIGHT': '240',
        'AUDIO_RATE': '32000' if soundtrack else '0',
        'CHANNELS': '1' if soundtrack else '0',
        'DCMV_CONTAINER': 'frames',
        'COMPRESSION_BACKEND': 'lz4',
        'USE_DEDUP': 'false',
        'SKIP_IF_EXISTS': 'false',
        'CLEANUP_TEMP': 'true',
        'THREADS': str(min(os.cpu_count() or 2, 8)),
        'FFMPEG_LOGLEVEL': 'error',
        'INTERMEDIATE_FORMAT': 'tga',
        # This pvrtex build requires an amount after --dither; the converter
        # script supplies the flag without one, so disable that script option.
        'PVRTX_DITHER': '0',
        # The upstream script expands this as words, not shell redirection.
        'PVRTX_QUIET': ' ',
    })
    for packer in ('pack_dcmv', 'pack_dcmv_chunk'):   # the vendored converter ships sources only
        if not (FMV / packer).is_file():
            run('gcc', '-O2', FMV / f'{packer}.c', '-o', FMV / packer, '-llz4', '-lzstd', '-lm')
    run(need(FMV / 'convert_to_pvr_fmv.sh'), cwd=FMV, env=env)
    need(target)


def boot_image(elf: Path, out: Path, stage: Path, pad_to: int = PAD_TO_MIB) -> None:
    """Update the executable and disc image from an existing staged tree."""
    if not stage.is_dir() or not (stage / 'data').is_dir():
        raise FileNotFoundError(stage)
    raw = out / 'main.bin'
    run('sh-elf-objcopy', '-R', '.stack', '-O', 'binary', elf, raw)
    run(need(KOS / 'utils/scramble/scramble'), raw, stage / '1ST_READ.BIN')
    ip_text = out / 'ip.txt'
    ip_text.write_text(
        'Hardware ID   : SEGA SEGAKATANA\n'
        'Maker ID      : SEGA ENTERPRISES\n'
        'Device Info   : 0000 CD-ROM1/1\n'
        'Area Symbols  : JUE\n'
        'Peripherals   : E000F10\n'
        'Product No    : SBRD0001\n'
        'Version       : V1.000\n'
        f'Release Date  : {datetime.date.today():%Y%m%d}\n'
        'Boot Filename : 1ST_READ.BIN\n'
        'SW Maker Name : SABER RIDER\n'
        'Game Title    : SABER RIDER STAR SHERIFFS\n')
    ip = out / 'IP.BIN'
    ip.unlink(missing_ok=True)  # makeip refuses to replace its own output
    run(need(KOS / 'utils/makeip/makeip'), ip_text, ip)
    iso = out / 'saber_rider.iso'
    # CLV: pad the image with a dummy file sorted first (innermost), so the game's files land at the outer part
    pad = stage / '0PADDING.BIN'
    pad.unlink(missing_ok=True)
    used = sum(f.stat().st_size for f in stage.rglob('*') if f.is_file()) + 4 * 1024 * 1024
    want = pad_to * 1024 * 1024 - used
    sort = out / 'sort.txt'
    sort.write_text(f'{pad} 1000\n')
    if pad_to > 0 and want > 0:
        with open(pad, 'wb') as f:   # sparse on the build machine; mkisofs writes the zeros
            f.truncate(want)
    run('mkisofs', '-quiet', '-V', 'SABER_RIDER', '-G', ip, '-joliet', '-rock', '-l', '-sort', sort, '-o', iso, stage)
    pad.unlink(missing_ok=True)
    cdi = out / 'saber_rider.cdi'
    run('cdi4dc', iso, cdi, '-d')
    if pad_to > 0:
        iso.unlink()   # a padded image is big: keep only the CDI
    print(f'Dreamcast disc: {cdi} ({cdi.stat().st_size / 1024 / 1024:.1f} MiB)', flush=True)


def build(args: argparse.Namespace) -> None:
    elf = need(args.elf.resolve())
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage, work = out / 'stage', out / 'work'
    if args.repack:
        boot_image(elf, out, stage, args.pad_to)
        return
    if args.data is None:
        raise ValueError('--data is required for a full disc build')
    data = args.data.resolve()
    for name in PACKS + ('video.pck',):
        need(data / name)
    if args.rebake:   # keep the converted music and videos, bake the data again
        if not (stage / 'music').is_dir():
            raise FileNotFoundError(stage / 'music')
        shutil.rmtree(stage / 'data', ignore_errors=True)
        shutil.rmtree(stage / 'sfx', ignore_errors=True)   # what older discs had instead of snd.pck / tex.pck
        for f in (stage / 'assets').rglob('*') if (stage / 'assets').is_dir() else ():
            if f.is_file() and f.suffix != '.dcmv':
                f.unlink()
        for d in sorted((stage / 'assets').rglob('*'), reverse=True) if (stage / 'assets').is_dir() else ():
            if d.is_dir() and not any(d.iterdir()):
                d.rmdir()
    else:
        fresh(stage)
    work.mkdir(exist_ok=True)

    # The runtime reads the five gameplay packs and the three baked ones; video.pck is 96 MB and is replaced by
    # independently seekable DCMV files in /video.
    (stage / 'data').mkdir()
    for name in PACKS:
        shutil.copy2(data / name, stage / 'data' / name)
    logf = open(out / 'bake.log', 'w')

    def log(msg: str) -> None:
        print(msg, file=logf, flush=True)

    tex, snd, files = pckwrite.Pack(), pckwrite.Pack(), pckwrite.Pack()
    bake_textures(data, work, tex, log)

    dcprep = out / 'dcprep'
    run('cc', '-O2', '-std=c11', '-Isrc', 'tools/dc/dcprep.c',
        'src/pack.c', 'src/lzo1z.c', 'src/platform/common/sfx_decode.c',
        'src/platform/common/mups.c', '-o', dcprep, cwd=ROOT)
    extracted = work / 'extracted'
    fresh(extracted)
    for kind in ('sfx', 'music', 'video'):
        folder = extracted / kind
        folder.mkdir()
        run(dcprep, kind, data, folder)
    for source in sorted((extracted / 'sfx').glob('*.wav')):
        snd.add(int(source.stem, 16), 'sample', sample_block(source, work))

    # Our files: WAVs to snd.pck, the rest to files.pck (a clip's .m4v name stays as an empty entry so asset_path finds
    # it; the video itself is the .dcmv file next to where it would be).
    for source in sorted((ROOT / 'assets').rglob('*')):
        if not source.is_file():
            continue
        rel = source.relative_to(ROOT / 'assets')
        if unused(rel):
            continue
        key, ext = namehash(rel.as_posix()), source.suffix.lower()
        if ext == '.wav':
            snd.add(key, 'sample', sample_block(source, work))
        elif ext == '.png':
            if rel.as_posix() in IMAGES:
                files.add(key, 'image', image_block(source))
        elif ext == '.m4v':
            files.add(key, 'file', file_block(b''))
        else:
            files.add(key, 'file', file_block(source.read_bytes()))
    for name, pack in (('tex.pck', tex), ('snd.pck', snd), ('files.pck', files)):
        size = pack.write(stage / 'data' / name)
        print(f'{name}: {len(pack.blocks)} blocks, {size / 1024 / 1024:.1f} MiB', flush=True)
    logf.close()

    if args.rebake:
        boot_image(elf, out, stage, args.pad_to)
        return
    for source in sorted((extracted / 'music').glob('*.ogg')):
        target = stage / 'music' / (source.stem + '.adx')
        target.parent.mkdir(exist_ok=True)
        # The pack's reconstructed Ogg pages preserve odd granule timestamps.
        # Decode to PCM first so the ADX muxer receives a clean sample clock.
        pcm = work / 'music.pcm.wav'
        run('ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
            '-i', source, '-ac', '2', '-ar', '44100', '-c:a', 'pcm_s16le', pcm)
        run('ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
            '-i', pcm, '-c:a', 'adpcm_adx', target)

    if not args.skip_fmv:
        for source in sorted((extracted / 'video').glob('*.m4v')):
            audio = next((p for p in (source.with_suffix('.ogg'), source.with_suffix('.wav')) if p.exists()), None)
            convert_video(source, audio, stage / 'video' / (source.stem + '.dcmv'),
                          25, work / 'fmv')
        for source in sorted((ROOT / 'assets/power').glob('*.m4v')):
            convert_video(source, None, stage / 'assets/power' / (source.stem + '.dcmv'),
                          24, work / 'fmv')

    boot_image(elf, out, stage, args.pad_to)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--data', type=Path, help='original demo data directory')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repack', action='store_true', help='reuse staged assets and rebuild only the boot image')
    parser.add_argument('--rebake', action='store_true',
                        help='bake the textures, samples and files again, reuse the staged music and videos')
    parser.add_argument('--skip-fmv', action='store_true', help='fast boot smoke test only')
    parser.add_argument('--pad-to', type=int, default=PAD_TO_MIB, metavar='MIB',
                        help=f'pad the image to this size so the data sits at the outer edge (default {PAD_TO_MIB}, 0: no padding)')
    args = parser.parse_args()
    try:
        build(args)
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
        print(f'disc build failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
