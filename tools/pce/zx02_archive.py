"""Sector-aligned ZX02 blocks, decoded directly to Arcade Card RAM.

Compressed 16 KiB blocks are read in batches through ADPCM RAM.
Incompressible blocks retain the existing two-bank raw transfer path.
"""
import hashlib
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

BLOCK = 16384


def decoder(data):
    """Independent host decoder, also verifies overlapping backreferences."""
    pos = 0
    bits = 0
    mask = 0
    extra = None
    output = bytearray()
    offset = 1

    def byte():
        nonlocal pos
        value = data[pos]; pos += 1
        return value

    def bit():
        nonlocal bits, mask, extra
        if extra is not None:
            value = extra; extra = None
            return value
        if not mask:
            bits = byte(); mask = 128
        value = bool(bits & mask); mask >>= 1
        return value

    def elias():
        value = 1
        while bit():
            value = value * 2 + bit()
        return value

    state = 0
    while True:
        if state == 0:
            for _ in range(elias()): output.append(byte())
            state = 2 if bit() else 1
        else:
            if state == 2:
                high = elias()
                if high == 256: return bytes(output)
                low = byte(); extra = low & 1
                offset = ((high - 1) << 7) + (low >> 1) + 1
            length = elias() + (state == 2)
            if length > 256: length &= 255
            for _ in range(length): output.append(output[-offset])
            state = 2 if bit() else 0


def pack_archives(out, names):
    source = Path(__file__).resolve().parents[3] / 'PCE/zx02/src'
    cache = out / 'work/zx02-cache'; cache.mkdir(parents=True, exist_ok=True)
    binary = cache / 'zx02'
    sources = [source / f'{n}.c' for n in ('zx02', 'compress', 'optimize', 'memory')]
    if not binary.exists() or any(p.stat().st_mtime > binary.stat().st_mtime for p in sources):
        subprocess.run(['cc', '-O3', '-DNDEBUG', *map(str, sources), '-o', str(binary)], check=True)

    def chunk(raw):
        digest = hashlib.sha256(raw).hexdigest()
        target = cache / f'{digest}.zx02'
        if not target.exists():
            original = cache / f'{digest}.raw'; original.write_bytes(raw)
            subprocess.run([str(binary), '-f', '-q', str(original), str(target)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        compressed = target.read_bytes()
        assert decoder(compressed) == raw, 'ZX02 block failed round trip'
        if len(compressed) >= len(raw): return raw, True
        return compressed, False

    report = {}
    for name in names:
        raw = (out / name).read_bytes()
        blocks = [raw[i:i + BLOCK] for i in range(0, len(raw), BLOCK)]
        unique = list(dict.fromkeys(blocks))
        with ThreadPoolExecutor(max_workers=4) as pool:
            encoded = dict(zip(unique, pool.map(chunk, unique)))
        packed = [encoded[b] for b in blocks]
        sizes = [((len(data) + 2047) // 2048) | (128 if plain else 0)
                 for data, plain in packed]
        assert len(sizes) <= 128
        header = b'ZXA1' + struct.pack('<H', len(sizes)) + bytes(sizes)
        result = header.ljust(2048, b'\0')
        result += b''.join(data.ljust((size & 127) * 2048, b'\0')
                           for (data, _), size in zip(packed, sizes))
        target = out / f'{Path(name).stem}_packed.bin'; target.write_bytes(result)
        report[name] = dict(raw_bytes=len(raw), disc_bytes=len(result), blocks=len(sizes),
                            raw_blocks=sum(plain for _, plain in packed))
    return report
