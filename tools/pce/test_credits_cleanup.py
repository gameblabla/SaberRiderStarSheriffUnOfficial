#!/usr/bin/env python3
"""Check every native credits page for leftover names, on PCE or SGX."""
import argparse
import re
import struct
import tempfile
from pathlib import Path
from emulator import Emulator, boot, symbol
from test_campaign import Campaign


def verify(out, sgx=False):
    header = (out / 'assets.h').read_text()
    offset = int(re.search(r'#define PCE_UI_CREDITS (\d+)', header)[1])
    raw = (out / 'ui.bin').read_bytes()
    count = struct.unpack_from('<H', raw, offset)[0]
    offsets = struct.unpack_from(f'<{count}H', raw, offset + 2)
    pages = [raw[offset + pos:].split(b'\0', 1)[0].decode('ascii') for pos in offsets]
    t = Campaign(out, sgx=sgx)
    ui = symbol(out / 'app.elf', 'pce_ui_state')
    with tempfile.TemporaryDirectory(prefix='credits-', dir=out) as base, Emulator(out / 'saber_rider.cue', base, sgx=sgx) as e:
        boot(e, t.address)
        t.dialogs(e)
        t.field(e, 'state', 4)  # Native ending path opens the credits.
        t.until(e, lambda: e.memory(ui, 1) == b'\4', limit=12000, step=1)
        for page, text in enumerate(pages):
            lines = text.split('\n')
            expected = [[0xc080] * 32 for _ in range(16)]
            row = 12 - len(lines) // 2
            for line in lines:
                col = (32 - len(line)) // 2
                for i, char in enumerate(line):
                    expected[row - 7][col + i] = ((13 if page < 2 else 12) << 12) | (0x80 + max(0, ord(char) - 32))
                row += 1
            def read_rows():
                bat = bytes.fromhex(e.call('asread', 'vram0', 7 * 64 * 2, 16 * 64 * 2)['hex'])
                return [list(struct.unpack_from('<32H', bat, (r * 64 + 4) * 2)) for r in range(16)]
            # Allow the native clear/draw to finish, but stop before the next
            # 210-frame page. Full BAT equality checks even empty name rows.
            for _ in range(190):
                actual = read_rows()
                if actual == expected:
                    break
                e.run(1)
            else:
                differences = [(r + 7, c + 4, actual[r][c], expected[r][c])
                               for r in range(16) for c in range(32) if actual[r][c] != expected[r][c]]
                raise AssertionError(('stale credits text', page, differences[:20]))
            if page in (6, 7, 84, 85):
                e.screenshot(out / f'credits-cleanup-page{page}.png')
            # Move near the next page, then let its native drawing complete.
            e.run(200)
    print(f'{"SGX" if sgx else "PCE"}: all {count} credits pages match, including cleared rows')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/pce'))
    parser.add_argument('--sgx', action='store_true')
    args = parser.parse_args()
    verify(args.out.resolve(), args.sgx)
