"""CUE LZ40 (`-ewl`, WRAM, low-endian) compressor for the Dreamcast.

Byte format (see src/platform/dreamcast/dcfmv/lz40.h and the vendored SH-4
reference src/platform/dreamcast/dcfmv/LZ40_dec.asm, MIT (c) 2024 VincentNL;
original LZX/LZ40 format by CUE, https://www.romhacking.net/utilities/826/):

  [0x40][declen lo][declen mid][declen hi] + flag bytes (stored negated:
  stored = (-logical) & 0xFF, 1 = match, MSB first) + symbols, + terminator
  (one dummy match flag bit + 2 zero bytes, never decoded).

Matches (offset 1..4095, WRAM allows 1):
  short:  2 bytes [((off&0xF)<<4)|len, off>>4]                 len 2..15
  medium: 3 bytes [((off&0xF)<<4), off>>4, len-0x10]           len 16..271
  long:   4 bytes [((off&0xF)<<4)|1, off>>4, lo, hi]           len 272..65807

This encoder is greedy (no CUE lazy evaluation) with threshold 3 (CUE uses 2;
emitting a literal where a 2-byte match would do is always valid, just a hair
larger) and a 3-byte hash chain (depth-limited, 4 KB window). It produces
byte-identical-format streams that lz40.h / the SH-4 ASM / CUE `lzx -d`
decode. Requires numpy (already required by build_disc.py) for C-speed match
lengths; falls back to a pure-python loop otherwise.
"""
from __future__ import annotations

N = 0x1000          # window (offsets 1..0xFFF)
F = 0x10            # 16: medium threshold base
F1 = 0x110          # 272: long threshold base
F2 = 0x10110        # 65808: max coded + 1
MAX_MATCH = F2 - 1  # 65807
THRESHOLD = 3       # greedy threshold (CUE uses 2; 3 is a valid subset)
HASH_DEPTH = 8      # candidates checked per position (most recent first)
HASH_KEEP = 32      # positions remembered per 3-byte key

try:
    import numpy as _np
except ImportError:  # pragma: no cover
    _np = None


def _match_len_np(arr, i: int, j: int, maxlen: int) -> int:
    a = arr[i:i + maxlen]
    b = arr[j:j + maxlen]
    ne = _np.not_equal(a, b)
    if not _np.any(ne):
        return int(len(a))
    return int(_np.argmax(ne))


def _match_len_py(data: bytes, i: int, j: int, maxlen: int) -> int:
    n = len(data)
    if i + maxlen > n:
        maxlen = n - i
    if j + maxlen > n:
        maxlen = n - j
    k = 0
    # compare 8 at a time via slicing (C-level memcmp) then binary refine?
    # simple loop is fine for the fallback path (small blocks only).
    while k < maxlen and data[i + k] == data[j + k]:
        k += 1
    return k


def lz40_compress(data: bytes) -> bytes:
    n = len(data)
    if n > 0xFFFFFF:
        raise ValueError('LZ40 declen is 24-bit')
    out = bytearray()
    out += bytes((0x40, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF))
    if n == 0:
        # CUE emits one flag byte (terminator bit) + 2 zero bytes.
        out += bytes(((-0x80) & 0xFF, 0, 0))
        return bytes(out)

    arr = _np.frombuffer(data, dtype=_np.uint8) if _np is not None else None
    match_fn = (_match_len_np if _np is not None else _match_len_py)

    table: dict[bytes, list[int]] = {}

    def table_add(pos: int) -> None:
        if pos + 2 >= n:
            return
        key = data[pos:pos + 3]
        lst = table.get(key)
        if lst is None:
            table[key] = [pos]
        else:
            lst.append(pos)
            if len(lst) > HASH_KEEP:
                del lst[0:len(lst) - HASH_KEEP]

    def best_at(i: int):
        """longest match at i within the 4 KB window (offset 1..0xFFF)."""
        if i + 2 >= n:
            return 0, 0
        key = data[i:i + 3]
        cands = table.get(key)
        if not cands:
            return 0, 0
        maxlen = n - i
        if maxlen > MAX_MATCH:
            maxlen = MAX_MATCH
        best_len, best_pos = 0, 0
        checked = 0
        for c in range(len(cands) - 1, -1, -1):
            j = cands[c]
            off = i - j
            if off <= 0 or off >= N:
                continue
            # first 3 bytes match by construction (same key)
            if arr is not None:
                ln = _match_len_np(arr, i, j, maxlen)
            else:
                ln = _match_len_py(data, i, j, maxlen)
            if ln > best_len:
                best_len, best_pos = ln, off
                if ln == maxlen or ln >= MAX_MATCH:
                    break
            checked += 1
            if checked >= HASH_DEPTH:
                break
        return best_len, best_pos

    flg_idx: int | None = None
    mask = 0
    logical = 0

    def flag_new() -> None:
        nonlocal flg_idx, mask, logical
        if flg_idx is not None:
            out[flg_idx] = (-logical) & 0xFF
        flg_idx = len(out)
        out.append(0)
        mask = 0x80
        logical = 0

    i = 0
    # prime: no positions yet; table fills as we go (matches only look back)
    while i < n:
        mask >>= 1
        if mask == 0:
            flag_new()
        else:
            # mask already holds this symbol's bit (shifted at top)
            pass
        ln, off = best_at(i)
        if ln >= THRESHOLD:
            if ln > MAX_MATCH:
                ln = MAX_MATCH
            logical |= mask
            if ln <= 0xF:
                out += bytes((((off & 0xF) << 4) | ln, (off >> 4) & 0xFF))
            elif ln <= 0x10F:
                out += bytes((((off & 0xF) << 4), (off >> 4) & 0xFF, (ln - 0x10) & 0xFF))
            else:
                v = ln - 0x110
                out += bytes((((off & 0xF) << 4) | 1, (off >> 4) & 0xFF, v & 0xFF, (v >> 8) & 0xFF))
            for k in range(ln):
                table_add(i + k)
            i += ln
        else:
            out.append(data[i])
            table_add(i)
            i += 1

    # terminator: one dummy match bit + 2 zero bytes (never decoded: the
    # decoder stops at declen). Mirrors CUE's `mask>>=1 / new-flag-if-empty`.
    mask >>= 1
    if mask == 0:
        flag_new()
    logical |= mask
    assert flg_idx is not None
    out[flg_idx] = (-logical) & 0xFF
    out += bytes((0, 0))
    return bytes(out)


def lz40_decompress(data: bytes) -> bytes:
    """Python mirror of lz40.h (for roundtrip tests)."""
    if len(data) < 4 or data[0] != 0x40:
        raise ValueError('not LZ40')
    declen = data[1] | (data[2] << 8) | (data[3] << 16)
    ip = 4
    end = len(data)
    out = bytearray()
    flags, mask = 0, 0
    while len(out) < declen:
        mask >>= 1
        if mask == 0:
            if ip >= end:
                raise ValueError('truncated flags')
            flags = (-data[ip]) & 0xFF
            ip += 1
            mask = 0x80
        if not (flags & mask):
            if ip >= end:
                raise ValueError('truncated literal')
            out.append(data[ip])
            ip += 1
        else:
            if ip + 2 > end:
                raise ValueError('truncated match')
            pos = data[ip] | (data[ip + 1] << 8)
            ip += 2
            tmp = pos & 0xF
            if tmp >= 2:
                ln = tmp
                pos >>= 4
            else:
                if ip >= end:
                    raise ValueError('truncated match len')
                ln = data[ip]
                ip += 1
                th = 0x10
                if tmp == 1:
                    if ip >= end:
                        raise ValueError('truncated match len2')
                    ln |= data[ip] << 8
                    ip += 1
                    th = 0x110
                ln += th
                pos >>= 4
            if pos == 0 or pos > len(out):
                raise ValueError('bad offset')
            if len(out) + ln > declen:
                raise ValueError('overflow')
            for _ in range(ln):
                out.append(out[-pos])
    return bytes(out)
