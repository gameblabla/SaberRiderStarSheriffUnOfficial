#!/usr/bin/env python3
"""Per-function CPU profile of the native game, using the emulator's cycle
profiler (see emulator-profile.patch). Usage:
    profile.py KEYS FRAMES [FUNCTION]
KEYS is a PCE pad bitmask held while profiling stage 1 (32 = right).
Percentages are of all master-clock cycles, including idle video_wait."""
import bisect, collections, subprocess, sys, tempfile
from pathlib import Path
from emulator import Emulator, boot, ROOT
from test_campaign import Campaign

def symbols(elf):
    text = subprocess.check_output([str(ROOT/'PCE/llvm-mos8/bin/llvm-nm'), '-n', str(elf)], text=True)
    table = []
    for line in text.splitlines():
        f = line.split()
        if len(f) == 3:
            a = int(f[0], 16); table.append(((((a >> 16) & 255) << 13) | (a & 0x1fff), f[2]))
    table.sort(); return table

def main():
    keys = int(sys.argv[1]) if len(sys.argv) > 1 else 32
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    out = Path(__file__).resolve().parents[2] / 'build/pce'
    table = symbols(out/'app.elf'); starts = [a for a, _ in table]
    t = Campaign(out)
    with tempfile.TemporaryDirectory(prefix='prof-', dir=out) as base, Emulator(out/'saber_rider.cue', base) as e:
        boot(e, t.address); e.run(300)
        t.seed(e, 'dialogs_done', 255, 1); t.field(e, 'state', 0); e.run(60)
        e.input(keys); e.run(60); before = t.metrics(e)['frames']
        dump = Path(base)/'prof.txt'
        e.call('prof_start'); e.run(frames); e.call('prof_dump', str(dump))
        print('loops', t.metrics(e)['frames'] - before, 'in', frames, 'emulator frames')
    total = 0; by_name = collections.Counter()
    for line in dump.read_text().splitlines():
        address, cycles = line.split(); address = int(address, 16); cycles = int(cycles); total += cycles
        i = bisect.bisect_right(starts, address) - 1
        by_name[table[i][1] if i >= 0 else '?'] += cycles
    for name, cycles in by_name.most_common(25): print(f'{cycles*100/total:6.2f}% {name}')

if __name__ == '__main__': main()
