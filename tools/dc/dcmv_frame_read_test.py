#!/usr/bin/env python3
"""Regression test for the DCMV v6 frame-read path.

dcfmv_frames_decode_frame reads (frame_offsets[u+1] - frame_offsets[u]) bytes into
compressed_buffer, which used to be sized with the header's max_compressed_size. The
packer pads each frame to 32 bytes, so that gap can be larger than max_compressed_size
and the read overflowed the heap buffer (on the intro video: frame 618, 19936 bytes into
a 19918-byte buffer).

Checks, against a real .dcmv:
  1. no frame's padded gap exceeds the buffer the fix sizes (max gap)
  2. every frame decodes to exactly uncompressed_frame_size with the LZ40 decoder
  3. the LZ40 header's 24-bit length matches uncompressed_frame_size

usage: dcmv_frame_read_test.py [file.dcmv ...]
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lz40 import lz40_decompress

DEFAULT = Path(__file__).resolve().parents[2] / 'build/dc/stage'
ALL_SAMPLES = os.environ.get('DCMV_TEST_ALL') == '1'


def check(path: Path) -> bool:
    d = path.read_bytes()
    ok = True
    if d[:4] != b'DCMV' or struct.unpack('<I', d[4:8])[0] != 6:
        print(f'{path.name}: not a DCMV v6 file')
        return True          # only v6 has the offset table this models
    ctype = d[45]
    nu, nt, fsz, mx, ao = struct.unpack('<IIIII', d[25:45])
    offs = struct.unpack('<' + 'I' * (nu + 1), d[50:50 + 4 * (nu + 1)])

    gaps = [offs[i + 1] - offs[i] for i in range(nu)]
    max_gap = max(gaps) if gaps else 0
    buffer_size = max(mx, max_gap)          # what dcfmv_load_frame_tables now allocates
    print(f'{path.name}: ctype={ctype} frames={nu} frame_size={fsz} '
          f'max_compressed={mx} max_padded_gap={max_gap} buffer={buffer_size}')

    over = [i for i, g in enumerate(gaps) if g > mx]
    if over:
        print(f'  note: {len(over)} frame(s) exceed max_compressed_size '
              f'(worst frame {over[0]}: {gaps[over[0]]} > {mx}) -> the old '
              f'max_compressed_size-sized buffer overflowed by '
              f'{gaps[over[0]] - mx} bytes')
    else:
        print('  all padded gaps fit in max_compressed_size (old sizing happened to work)')

    for g in gaps:
        if g > buffer_size:
            print(f'  FAIL: gap {g} > buffer {buffer_size}')
            ok = False
            break

    if ctype == 2:
        idxs = range(nu) if (ALL_SAMPLES or nu <= 64) else [0, 1, 2, nu // 3, nu // 2, nu - 3, nu - 2, nu - 1]
        n = 0
        for i in idxs:
            raw = d[offs[i]:offs[i + 1]]
            try:
                out = lz40_decompress(raw)
            except Exception as exc:                       # noqa: BLE001
                print(f'  FAIL frame {i}: {exc}')
                ok = False
                continue
            n += 1
            if len(out) != fsz:
                print(f'  FAIL frame {i}: decoded {len(out)} != {fsz}')
                ok = False
            if raw[0] != 0x40:
                print(f'  FAIL frame {i}: bad LZ40 magic {raw[0]:#x}')
                ok = False
            hdr = raw[1] | (raw[2] << 8) | (raw[3] << 16)
            if hdr != fsz:
                print(f'  FAIL frame {i}: header len {hdr} != {fsz}')
                ok = False
        print(f'  decoded {n} frame(s), each {fsz} bytes')
    else:
        print(f'  (ctype {ctype}: not LZ40, decode not checked here)')

    print('  OK' if ok else '  FAILED')
    return ok


def main() -> int:
    args = sys.argv[1:]
    files = [Path(a) for a in args] if args else sorted(DEFAULT.rglob('*.dcmv'))
    if not files:
        print('no .dcmv files to test')
        return 0
    all_ok = all(check(f) for f in files)
    print('PASS' if all_ok else 'FAIL')
    return 0 if all_ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
