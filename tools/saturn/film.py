"""The Saturn's videos (plan 6): Cinepak frames with their share of the soundtrack, as "SCPK" files the player
(src/platform/saturn/video_sat.c) streams from the CD.

ffmpeg does the work: the picture scaled / cropped to the size the screen shows it at and brought to FPS (the SH-2s
decode Cinepak at up to 20 frames a second), encoded as Cinepak (in a Sega FILM file, from which the frames are taken);
the sound as 16-bit mono PCM at RATE. The file:

  header (big-endian, padded to whole 2048-byte sectors): "SCPK", u16 version 1, u16 header sectors, u16 w, h,
    u16 fps num, den, u32 video frames, u32 chunks, u32 audio rate (0: silent), u16 lead frames, u16 bits (16),
    u32 largest chunk, u32 chunk size[chunks]
  chunk k (k < frames): u32 Cinepak bytes, u32 audio bytes, the frame padded to 4 bytes, the samples: chunk 0 carries
    the sound of frames 0 .. LEAD (it primes the player's sound ring), chunk k the sound of frame k + LEAD
  then, when the sound outlasts the picture, one more chunk with no picture: the rest of the sound (the power clips'
    voices run past their pictures; the player lets it play out)
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FPS = 20
RATE = 22050
LEAD = 10          # frames of sound ahead of the picture (half a second)


def ffmpeg_exe() -> str:
    exe = shutil.which('ffmpeg')
    if exe:
        return exe
    import imageio_ffmpeg   # pip: a static ffmpeg with the Cinepak encoder and the film_cpk muxer
    return imageio_ffmpeg.get_ffmpeg_exe()


def film_frames(path: Path) -> list[bytes]:
    """the video samples of a Sega FILM file, in order"""
    d = path.read_bytes()
    assert d[:4] == b'FILM', path
    head = struct.unpack_from('>I', d, 4)[0]
    p, frames = 16, []
    while p < head:
        tag, size = d[p:p + 4], struct.unpack_from('>I', d, p + 4)[0]
        if tag == b'STAB':
            n = struct.unpack_from('>I', d, p + 12)[0]
            for i in range(n):
                off, length, info1, _ = struct.unpack_from('>IIII', d, p + 16 + 16 * i)
                if info1 != 0xFFFFFFFF:   # audio samples have info1 all ones
                    frames.append(d[head + off:head + off + length])
        p += size
    return frames


def make(source: Path, out: Path, work: Path, size: tuple[int, int], vf: str, in_args: list[str] = (),
         audio: Path | None = None, log=print) -> None:
    """source (video; its sound unless `audio`) -> out (SCPK); vf: the ffmpeg filters before the size (crop, pad)"""
    w, h = size
    assert w % 8 == 0 and h % 4 == 0 and h <= 255, size
    key = hashlib.sha1(json.dumps([str(source), source.stat().st_size, source.stat().st_mtime, str(audio),
                                   audio.stat().st_size if audio else 0, size, vf, list(in_args), FPS, RATE, LEAD, 1]).encode()).hexdigest()[:16]
    cached = work / f'{out.stem}.{key}.cpk'
    if cached.exists():
        shutil.copy2(cached, out)
        return
    work.mkdir(parents=True, exist_ok=True)
    ff = [ffmpeg_exe(), '-nostdin', '-hide_banner', '-loglevel', 'error', '-y']
    film = work / f'{out.stem}.film'
    filters = ','.join(f for f in (vf, f'scale={w}:{h}:flags=lanczos', f'fps={FPS}') if f)
    subprocess.run(ff + list(in_args) + ['-i', str(source), '-an', '-vf', filters, '-pix_fmt', 'rgb24',
                                         '-c:v', 'cinepak', '-max_strips', '3', '-f', 'film_cpk', str(film)], check=True)
    frames = film_frames(film)
    pcm = b''
    if audio is not False:
        raw = work / f'{out.stem}.pcm'
        src = audio if audio else source
        r = subprocess.run(ff + ([] if audio else list(in_args)) + ['-i', str(src), '-vn', '-ac', '1', '-ar', str(RATE),
                                                                      '-f', 's16be', str(raw)])
        pcm = raw.read_bytes() if r.returncode == 0 and raw.exists() else b''
    n = len(frames)
    per = RATE / FPS
    edge = lambda k: int(round(k * per)) * 2   # byte offset of frame k's sound
    rate = RATE if pcm else 0
    if pcm and len(pcm) < edge(n + LEAD):
        pcm += bytes(edge(n + LEAD) - len(pcm))   # silence to the picture's end (the player's ring must never run dry)
    chunks = []
    for k, f in enumerate(frames):
        a = pcm[edge(k + LEAD if k else 0):edge(k + 1 + LEAD)] if pcm else b''
        chunks.append(struct.pack('>II', len(f), len(a)) + f + bytes(-len(f) % 4) + a)
    if pcm and len(pcm) > edge(n + LEAD):
        tail = pcm[edge(n + LEAD):]
        chunks.append(struct.pack('>II', 0, len(tail)) + tail)
    head = struct.pack('>4sHHHHHHIIIHHI', b'SCPK', 1, 0, w, h, FPS, 1, n, len(chunks), rate, LEAD, 16,
                       max(len(c) for c in chunks)) + b''.join(struct.pack('>I', len(c)) for c in chunks)
    sectors = -(-len(head) // 2048)
    head = head[:6] + struct.pack('>H', sectors) + head[8:]
    head += bytes(sectors * 2048 - len(head))
    body = b''.join(chunks)
    out.write_bytes(head + body)
    shutil.copy2(out, cached)
    film.unlink()
    secs = n / FPS
    log(f'video {out.name}: {w}x{h}, {n} frames ({secs:.1f} s), {len(body) // 1024} KB, '
        f'{len(body) / secs / 1024 if secs else 0:.0f} KB/s, largest chunk {max(len(c) for c in chunks) // 1024} KB, sound {rate} Hz')
