"""Build libyaul_cinepak-compatible Sega FILM/CPK movies with ADX audio.

The libyaul decoder consumes the Sega Cinepak frame header as 12 bytes: the usual
10-byte Cinepak header plus the 16-bit Sega padding field.  Do not strip bytes
10..11 from the Cinepak samples; ffmpeg's film_cpk muxer emits the variant the
Saturn player expects.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess

FPS = 15
RATE = 22050
MAX_STRIPS = 2       # libyaul_cinepak has two strip codebooks
CACHE_VERSION = 4    # CPK/FILM + ADX, continuous audio timestamps


def ffmpeg_exe() -> str:
    exe = shutil.which('ffmpeg')
    if exe:
        return exe
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def _be32(data: bytes, off: int) -> int:
    return struct.unpack_from('>I', data, off)[0]


def _video_samples(path: Path) -> tuple[int, int, int, list[tuple[bytes, int, int]]]:
    """Return video samples from ffmpeg's FILM file, preserving the full Sega header."""
    data = path.read_bytes()
    if data[:4] != b'FILM' or data[16:20] != b'FDSC' or data[48:52] != b'STAB':
        raise RuntimeError(f'{path}: ffmpeg did not produce a Sega FILM file')
    head_size = _be32(data, 4)
    height, width = _be32(data, 28), _be32(data, 32)
    ticks, n = _be32(data, 56), _be32(data, 60)
    frames: list[tuple[bytes, int, int]] = []
    for i in range(n):
        off, length, time, duration = struct.unpack_from('>IIII', data, 64 + i * 16)
        if time == 0xFFFFFFFF:
            continue
        frame = data[head_size + off:head_size + off + length]
        if len(frame) != length or len(frame) < 12:
            raise RuntimeError(f'{path}: truncated Cinepak frame {i}')
        # libyaul film_cvid.c's videoHeader is 12 bytes, including padding at +10.
        strips = struct.unpack_from('>H', frame, 8)[0]
        if strips > MAX_STRIPS:
            raise RuntimeError(f'{path}: frame uses {strips} Cinepak strips (max {MAX_STRIPS})')
        frames.append((frame, time, duration))
    if not frames:
        raise RuntimeError(f'{path}: no Cinepak frames')
    return width, height, ticks, frames


def _adx(path: Path) -> tuple[bytes, bytes, int, int, int]:
    """Return ADX header, compressed block data, channels, rate, samples-per-channel.

    ffmpeg writes an 18-byte 0x8001 end marker.  libyaul's ADX path decodes every
    block it is handed, so remove that marker instead of turning it into a loud
    bogus PCM block.
    """
    data = path.read_bytes()
    if len(data) < 36 or data[:2] != b'\x80\x00':
        raise RuntimeError(f'{path}: not ADX')
    header_size = struct.unpack_from('>H', data, 2)[0] + 4
    fmt, block_size, bits, channels = data[4:8]
    rate = _be32(data, 8)
    cutoff = struct.unpack_from('>H', data, 16)[0]
    if (fmt, block_size, bits, cutoff) != (3, 18, 4, 500) or channels not in (1, 2):
        raise RuntimeError(f'{path}: ADX must be fixed-coefficient 4-bit/18-byte/500-Hz mono or stereo')
    payload = data[header_size:]
    # ADX terminator is one block, irrespective of channel count.
    if len(payload) >= 18 and payload[-18:-16] == b'\x80\x01':
        payload = payload[:-18]
    group = block_size * channels
    if len(payload) % group:
        raise RuntimeError(f'{path}: ADX payload is not aligned to {group}-byte channel groups')
    samples = (len(payload) // group) * 32
    return data[:header_size], payload, channels, rate, samples


def _mux(video_film: Path, adx_file: Path, out: Path) -> tuple[int, int, int, int, int]:
    width, height, ticks, frames = _video_samples(video_film)
    adx_header, adx_payload, channels, rate, audio_samples = _adx(adx_file)
    group_bytes = 18 * channels
    total_groups = len(adx_payload) // group_bytes
    # libyaul streams samples straight into a DMA ring.  Keep every sample
    # 32-bit aligned: its SCU DMA path loses the leading half-word when the
    # next sample starts at +2.  One all-zero ADX block is valid silence and
    # makes an odd block-group count even (<= 1.5 ms extra at 22.05 kHz).
    if total_groups & 1:
        adx_payload += bytes(group_bytes)
        total_groups += 1
        audio_samples += 32
    groups_done = 0
    samples: list[tuple[bytes, int, int]] = []

    # Put each interval's audio first. handle_play() can consume it and then parse
    # the following video in the same task, and an ordinary movie therefore ends
    # on a timed video sample rather than time==0xffffffff audio.
    for k, (frame, time, duration) in enumerate(frames):
        next_tick = (time & 0x7FFFFFFF) + max(duration, 1)
        target = min(total_groups, math.ceil((next_tick * rate / ticks) / 32.0))
        take = max(0, target - groups_done)
        if take & 1:
            take += 1
        take = min(take, total_groups - groups_done)
        if take:
            payload = (adx_header if groups_done == 0 else b'') + \
                      adx_payload[groups_done * group_bytes:(groups_done + take) * group_bytes]
            samples.append((payload, 0xFFFFFFFF, duration))
            groups_done += take
        # The decoder obeys the internal Cinepak packet length, so trailing
        # zeroes are harmless and keep the following FILM sample DMA-aligned.
        frame += bytes(-len(frame) % 4)
        samples.append((frame, time, duration))

    # ADX can outlast the picture (the power-up voices do). Keep tail entries
    # small enough for the streaming ring, then finish with a zero-strip timed
    # Cinepak sample so film_lib's end-time calculation never sees 0xffffffff.
    groups_per_tick = max(1, math.ceil((rate / ticks) / 32.0))
    while groups_done < total_groups:
        take = min(max(2, (groups_per_tick + 1) & ~1), total_groups - groups_done)
        if take & 1:
            take -= 1
        if take == 0:
            take = total_groups - groups_done
        payload = (adx_header if groups_done == 0 else b'') + \
                  adx_payload[groups_done * group_bytes:(groups_done + take) * group_bytes]
        samples.append((payload, 0xFFFFFFFF, 1))
        groups_done += take

    last_video_tick = (frames[-1][1] & 0x7FFFFFFF) + max(frames[-1][2], 1)
    audio_ticks = math.ceil(audio_samples * ticks / rate) if rate else 0
    end_tick = max(last_video_tick, audio_ticks)
    # flags/length=12, width, height, numStrips=0, Sega padding=0.
    # parseVideo() leaves the already decoded VDP1 surface untouched.
    noop = struct.pack('>IHHHH', 0x0100000C, width, height, 0, 0)
    samples.append((noop, 0x80000000 | end_tick, 1))

    entries: list[bytes] = []
    body = bytearray()
    offset = 0
    for payload, time, duration in samples:
        entries.append(struct.pack('>IIII', offset, len(payload), time, duration))
        body += payload
        offset += len(payload)

    stab_size = 16 + 16 * len(entries)
    header_size = 16 + 32 + stab_size
    fdsc = struct.pack('>4sI4sIIBBBBII', b'FDSC', 32, b'cvid', height, width,
                       24, channels, 16, 2, rate << 16, 0)
    head = (struct.pack('>4sIII', b'FILM', header_size, 0x312E3039, 0) + fdsc +
            struct.pack('>4sIII', b'STAB', stab_size, ticks, len(entries)) + b''.join(entries))
    blob = head + body
    blob += bytes(-len(blob) % 4)   # libyaul's CD path requires a 32-bit-sized file
    out.write_bytes(blob)
    return width, height, len(frames), len(entries), rate


def make(source: Path, out: Path, work: Path, size: tuple[int, int], vf: str,
         in_args: list[str] = (), audio: Path | None = None, log=print) -> None:
    """Convert a source movie to a libyaul Cinepak/ADX Sega FILM .CPK."""
    width, height = size
    if width % 8 or height % 4 or height > 255:
        raise ValueError(f'unsupported Cinepak size {size}')
    audio_path = audio if audio and audio is not False else None
    key_data = [str(source), source.stat().st_size, source.stat().st_mtime,
                str(audio_path), audio_path.stat().st_size if audio_path else 0,
                audio_path.stat().st_mtime if audio_path else 0,
                size, vf, list(in_args), FPS, RATE, MAX_STRIPS, CACHE_VERSION]
    key = hashlib.sha1(json.dumps(key_data).encode()).hexdigest()[:16]
    cached = work / f'{out.stem}.{key}.cpk'
    if cached.exists() and cached.stat().st_size:
        shutil.copy2(cached, out)
        return

    work.mkdir(parents=True, exist_ok=True)
    ff = [ffmpeg_exe(), '-nostdin', '-hide_banner', '-loglevel', 'error', '-y']
    video_film = work / f'{out.stem}.{key}.video.cpk'
    adx = work / f'{out.stem}.{key}.adx'
    filters = ','.join(x for x in (vf, f'scale={width}:{height}:flags=lanczos', f'fps={FPS}') if x)
    subprocess.run(ff + list(in_args) + ['-i', str(source), '-an', '-vf', filters, '-pix_fmt', 'rgb24',
                                        '-c:v', 'cinepak', '-max_strips', str(MAX_STRIPS),
                                        '-f', 'film_cpk', str(video_film)], check=True)

    audio_source = audio_path if audio_path else source
    audio_args = [] if audio_path else list(in_args)
    r = subprocess.run(ff + audio_args + ['-i', str(audio_source), '-vn', '-ac', '1', '-ar', str(RATE),
                                         '-af', 'asetpts=N/SR/TB',
                                         '-c:a', 'adpcm_adx', '-f', 'adx', str(adx)])
    if r.returncode != 0 or not adx.exists() or not adx.stat().st_size:
        # The game expects an audio-bearing FILM.  If a source is silent, encode
        # enough silence to clock the picture instead of reverting to raw PCM.
        _, _, ticks, frames = _video_samples(video_film)
        duration = ((frames[-1][1] & 0x7FFFFFFF) + max(frames[-1][2], 1)) / ticks
        subprocess.run(ff + ['-f', 'lavfi', '-i', f'anullsrc=r={RATE}:cl=mono', '-t', f'{duration:.6f}',
                             '-c:a', 'adpcm_adx', '-f', 'adx', str(adx)], check=True)

    w, h, nframes, nentries, rate = _mux(video_film, adx, out)
    shutil.copy2(out, cached)
    # ffprobe understands the output as a normal Sega FILM/Cinepak + ADX file.
    secs = nframes / FPS
    log(f'video {out.name}: {w}x{h}, {nframes} frames ({secs:.1f} s), {out.stat().st_size // 1024} KB, '
        f'{out.stat().st_size / secs / 1024 if secs else 0:.0f} KB/s, {nentries} samples, ADX {rate} Hz')
