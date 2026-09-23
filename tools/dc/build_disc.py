#!/usr/bin/env python3
"""Build a self-boot Dreamcast CDI from the KOS ELF and original demo packs.

The original E2DM packs are user supplied. All generated media stays under
--out; the source packs and assets are never modified.
"""
from __future__ import annotations

import argparse
import datetime
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
KOS = Path('/opt/toolchains/dc/kos')
FMV = ROOT / 'third_party/dreamcast-fmv'
PACKS = ('pack.pck', 'common.pck', 'levels.pck', 'menu.pck', 'level1.pck')


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


def convert_wav(source: Path, target: Path, work: Path) -> None:
    """KOS wav2adpcm needs mono PCM16; keep the source sample rate."""
    target.parent.mkdir(parents=True, exist_ok=True)
    pcm = work / 'sample.pcm.wav'
    run('ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
        '-i', source, '-ac', '1', '-c:a', 'pcm_s16le', pcm)
    run(need(KOS / 'utils/wav2adpcm/wav2adpcm'), '-t', pcm, target)


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


def boot_image(elf: Path, out: Path, stage: Path) -> None:
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
    run('mkisofs', '-quiet', '-V', 'SABER_RIDER', '-G', ip, '-joliet', '-rock', '-l', '-o', iso, stage)
    cdi = out / 'saber_rider.cdi'
    run('cdi4dc', iso, cdi, '-d')
    print(f'Dreamcast disc: {cdi} ({cdi.stat().st_size / 1024 / 1024:.1f} MiB)', flush=True)


def build(args: argparse.Namespace) -> None:
    elf = need(args.elf.resolve())
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stage, work = out / 'stage', out / 'work'
    if args.repack:
        boot_image(elf, out, stage)
        return
    if args.data is None:
        raise ValueError('--data is required for a full disc build')
    data = args.data.resolve()
    for name in PACKS + ('video.pck',):
        need(data / name)
    fresh(stage)
    work.mkdir(exist_ok=True)

    # The runtime reads only the five gameplay packs; video.pck is 96 MB and
    # is replaced by independently seekable DCMV files in /video.
    (stage / 'data').mkdir()
    for name in PACKS:
        shutil.copy2(data / name, stage / 'data' / name)

    # Copy the game's authored art and text. A WAV must be encoded for the
    # AICA, but the original .m4v names stay as tiny lookup placeholders:
    # video_open_file resolves the sibling .dcmv path.
    for source in sorted((ROOT / 'assets').rglob('*')):
        if not source.is_file():
            continue
        rel = source.relative_to(ROOT / 'assets')
        target = stage / 'assets' / rel
        if source.suffix.lower() == '.wav':
            convert_wav(source, target, work)
        elif source.suffix.lower() == '.m4v':
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'DCMV lookup placeholder\n')
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

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
        convert_wav(source, stage / 'sfx' / source.name, work)
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

    boot_image(elf, out, stage)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--data', type=Path, help='original demo data directory')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repack', action='store_true', help='reuse staged assets and rebuild only the boot image')
    parser.add_argument('--skip-fmv', action='store_true', help='fast boot smoke test only')
    args = parser.parse_args()
    try:
        build(args)
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
        print(f'disc build failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
