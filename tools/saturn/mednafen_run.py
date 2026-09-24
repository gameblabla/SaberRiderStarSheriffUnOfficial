#!/usr/bin/env python3
"""Run the Saturn build headless in the mednafen debug kit and report what it did (plan 11).

  mednafen_run.py [--cue build/saturn/saber_rider.cue] --frames 3600 \
      [--pad "600:start,610:none,900:a,905:none"] [--shots DIR --shot-every 60] [--wav out.wav] \
      [--profile prof.tsv --profile-from 1200 [--cpu 0|1]] [--state-in s.mcs] [--state-out s.mcs]

  * the game's printf output comes from its log ring (`saber_log` in the ELF, see src/platform/saturn/log_sat.c),
    read after every chunk of frames and printed with the frame number;
  * --pad holds Saturn pad buttons from a frame on (names joined by '+': up down left right a b c x y z l r start,
    or none), frame-exact: the kit's `pad` command;
  * --profile: exact per-instruction coverage of one SH-2 from --profile-from to the end, folded into per-function
    instruction counts with the ELF's symbols (SH-2 instructions are mostly 1 cycle; memory waits are not counted);
  * the BIOS boot takes ~430 frames before the game's first line.
The emulator is the kit's `mednafen-headless` (MEDNAFEN env var or --mednafen); it uses ~/.mednafen/firmware.
"""
from __future__ import annotations

import argparse
import bisect
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT.parent / 'Saturn/mednafen-headless-debug-kit/mednafen-headless'
NM = Path(os.environ.get('YAUL_INSTALL_ROOT', Path.home() / '.local/x-tools/sh2eb-elf')) / 'bin/sh2eb-elf-nm'


class Mednafen:
    def __init__(self, exe: Path, base: Path | None) -> None:
        cmd = [str(exe)] + (['--base', str(base)] if base else [])
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def call(self, *args: object) -> str:
        line = '\t'.join(str(a) for a in args)
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        reply = self.p.stdout.readline().rstrip('\n')
        if not reply.startswith('OK'):
            raise RuntimeError(f'{line!r}: {reply}')
        out, i, body = [], 0, reply[3:]   # the protocol escapes \\ \n \t \r
        while i < len(body):
            c = body[i]
            if c == '\\' and i + 1 < len(body):
                out.append({'n': '\n', 't': '\t', 'r': '\r'}.get(body[i + 1], body[i + 1])); i += 2
            else:
                out.append(c); i += 1
        return ''.join(out)

    def close(self) -> None:
        try:
            self.call('quit')
        except (RuntimeError, BrokenPipeError):
            pass
        self.p.wait(timeout=10)


def symbols(elf: Path) -> tuple[list[int], list[str], dict[str, int]]:
    out = subprocess.run([NM, '-n', elf], check=True, capture_output=True, text=True).stdout
    addrs, names, by_name = [], [], {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'tTwW':
            addrs.append(int(f[0], 16)); names.append(f[2])
        if len(f) == 3:
            by_name.setdefault(f[2], int(f[0], 16))
    return addrs, names, by_name


class LogRing:
    """the game's log ring: "SABERLOG", u32 size, u32 head, buf (big-endian words)"""
    def __init__(self, emu: Mednafen, addr: int) -> None:
        self.emu, self.addr, self.pos, self.partial = emu, addr, 0, ''

    def read(self) -> list[str]:
        space, off = ('workramh', self.addr - 0x06000000) if self.addr >= 0x06000000 else ('workraml', self.addr - 0x00200000)
        head = bytes.fromhex(self.emu.call('mem_read', space, off, 16))
        if head[:8] != b'SABERLOG':
            return []
        size, total = int.from_bytes(head[8:12], 'big'), int.from_bytes(head[12:16], 'big')
        if total == self.pos:
            return []
        start = max(self.pos, total - size)
        data = bytearray()
        for p in range(start, total, 4096):
            n = min(4096, total - p)
            first = p % size
            chunk = min(n, size - first)
            data += bytes.fromhex(self.emu.call('mem_read', space, off + 16 + first, chunk))
            if chunk < n:
                data += bytes.fromhex(self.emu.call('mem_read', space, off + 16, n - chunk))
        lost = start - self.pos
        self.pos = total
        text = self.partial + data.decode('latin-1')
        lines = text.split('\n')
        self.partial = lines.pop()
        return ([f'[log: {lost} bytes lost]'] if lost else []) + lines


def parse_pad(script: str) -> list[tuple[int, str]]:
    events = []
    for item in filter(None, (s.strip() for s in script.split(','))):
        frame, _, buttons = item.partition(':')
        events.append((int(frame), buttons or 'none'))
    return sorted(events)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cue', type=Path, default=ROOT / 'build/saturn/saber_rider.cue')
    ap.add_argument('--elf', type=Path, default=ROOT / 'obj-saturn/saber_rider.elf')
    ap.add_argument('--mednafen', type=Path, default=Path(os.environ.get('MEDNAFEN', KIT)))
    ap.add_argument('--base', type=Path, help='isolated mednafen base directory (parallel runs)')
    ap.add_argument('--frames', type=int, default=1200)
    ap.add_argument('--chunk', type=int, default=60, help='frames between log reads')
    ap.add_argument('--pad', default='', help='FRAME:BUTTONS,... held from that frame on')
    ap.add_argument('--shots', type=Path, help='directory for PNG screenshots')
    ap.add_argument('--shot-every', type=int, default=0)
    ap.add_argument('--shot-at', default='', help='comma-separated frames to screenshot')
    ap.add_argument('--wav', type=Path)
    ap.add_argument('--profile', type=Path, help='per-function instruction counts (TSV)')
    ap.add_argument('--profile-from', type=int, default=0)
    ap.add_argument('--cpu', type=int, default=0)
    ap.add_argument('--callers', action='store_true', help='with --profile: which functions call the soft-float routines '
                    '(and floorf & co.) how often, from the edge coverage (-> <profile>.callers.tsv)')
    ap.add_argument('--state-in', type=Path)
    ap.add_argument('--state-out', type=Path)
    ap.add_argument('--regs', action='store_true', help='at the end: both SH-2s\' PC / PR (with function names) and registers')
    ap.add_argument('--quiet', action='store_true', help='no log lines on stdout (still written with --log)')
    ap.add_argument('--log', type=Path, help='also write the game log here')
    args = ap.parse_args()

    addrs, names, by_name = symbols(args.elf)
    log_addr = by_name.get('_saber_log')
    emu = Mednafen(args.mednafen, args.base)
    logf = open(args.log, 'w') if args.log else None
    try:
        emu.call('load', args.cue.resolve(), 'ss')
        if args.state_in:
            emu.call('load_state', args.state_in.resolve())
        ring = LogRing(emu, log_addr) if log_addr else None
        pad = parse_pad(args.pad)
        shot_at = {int(x) for x in args.shot_at.split(',') if x.strip()}
        if args.shots:
            args.shots.mkdir(parents=True, exist_ok=True)
        if args.wav:
            emu.call('wav_start', args.wav.resolve())
        frame, profiling = 0, False
        if args.profile:
            emu.call('cpu', args.cpu)
            emu.call('coverage_clear')
        # stop points: pad events, screenshots, log chunks, the profile start
        while frame < args.frames:
            if args.profile and not profiling and frame >= args.profile_from:
                emu.call('coverage_start'); profiling = True
            while pad and pad[0][0] <= frame:
                emu.call('pad', 0, pad.pop(0)[1])
            nxt = min([args.frames, frame + args.chunk] + [f for f, _ in pad[:1] if f > frame] +
                      [f for f in shot_at if f > frame] +
                      ([args.profile_from] if args.profile and not profiling and args.profile_from > frame else []))
            if args.shot_every:
                nxt = min(nxt, (frame // args.shot_every + 1) * args.shot_every)
            want_shot = (args.shots and ((args.shot_every and nxt % args.shot_every == 0) or nxt in shot_at))
            if nxt - frame > 1:
                emu.call('run', nxt - frame - 1, 1)
            emu.call('run', 1, 0 if want_shot else 1)
            frame = nxt
            if want_shot:
                emu.call('screenshot', (args.shots / f'f{frame:06d}.png').resolve())
            for line in ring.read() if ring else []:
                text = f'{frame:6d} | {line}'
                if not args.quiet:
                    print(text, flush=True)
                if logf:
                    print(text, file=logf, flush=True)
        if args.regs:
            def fn(a: int) -> str:
                i = bisect.bisect_right(addrs, a) - 1
                return f'{names[i]}+0x{a - addrs[i]:x}' if i >= 0 else '?'
            for cpu in (0, 1):
                emu.call('cpu', cpu)
                vals = {}
                for row in emu.call('regs').splitlines():
                    f = row.split('\t')
                    if len(f) >= 4:
                        vals[f[2].upper()] = int(f[3], 16)
                pc = vals.get('RPC', vals.get('PC', 0))
                print(f'cpu {cpu}: PC {pc:08x} {fn(pc)}  PR {vals.get("PR", 0):08x} {fn(vals.get("PR", 0))}')
                print('  ' + ' '.join(f'R{i}={vals.get(f"R{i}", 0):08x}' for i in range(16)))
            emu.call('cpu', 0)
        if args.wav:
            emu.call('wav_stop')
        if args.state_out:
            emu.call('save_state', args.state_out.resolve())
        if args.profile:
            emu.call('coverage_stop')
            raw = args.profile.with_suffix('.pc.tsv')
            emu.call('coverage_save', raw.resolve())
            if args.callers:
                edges = args.profile.with_suffix('.edges.tsv')
                emu.call('edge_save', edges.resolve())
            per: dict[str, int] = {}
            total = 0
            hits: list[tuple[int, int]] = []
            for line in raw.read_text().splitlines():
                f = line.split('\t')
                if len(f) < 2 or not f[0].strip().lower().startswith(('0x', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9')):
                    continue
                try:
                    pc, n = int(f[0], 16), int(f[1])
                except ValueError:
                    continue
                hits.append((pc, n))
                total += n
            # attribute to the innermost source function (LTO inlines most of the core into a few big symbols)
            a2l = Path(str(NM).replace('-nm', '-addr2line'))
            res = subprocess.run([a2l, '-f', '-e', args.elf], input='\n'.join(hex(pc) for pc, _ in hits),
                                 check=True, capture_output=True, text=True).stdout.splitlines()
            for k, (pc, n) in enumerate(hits):
                name = res[2 * k] if 2 * k < len(res) else '?'
                if name == '??':
                    i = bisect.bisect_right(addrs, pc) - 1
                    name = names[i] if i >= 0 else '?'
                src = res[2 * k + 1].split(':')[0].rsplit('/', 1)[-1] if 2 * k + 1 < len(res) else ''
                key = f'{name} ({src})' if src and src != '??' else name
                per[key] = per.get(key, 0) + n
            frames = max(1, args.frames - args.profile_from)
            with open(args.profile, 'w') as out:
                out.write('function\tinstructions\tper_frame\tshare\n')
                for name, n in sorted(per.items(), key=lambda kv: -kv[1]):
                    out.write(f'{name}\t{n}\t{n / frames:.0f}\t{100 * n / max(1, total):.2f}\n')
            print(f'profile: {total} instructions over {frames} frames = {total / frames:.0f} a frame -> {args.profile}')
            if args.callers:
                float_calls(edges, args, addrs, names, by_name, frames)
    finally:
        emu.close()
        if logf:
            logf.close()
    return 0


FLOAT_ENTRY = ('__addsf3', '__subsf3', '__mulsf3', '__divsf3', '__ltsf2', '__lesf2', '__gtsf2', '__gesf2', '__eqsf2',
               '__nesf2', '__fixsfsi', '__fixunssfsi', '__floatsisf', '__floatunsisf', '__extendsfdf2', '__truncdfsf2',
               'floorf', 'ceilf', 'truncf', 'roundf', 'fabsf', 'fminf', 'fmaxf', 'fmodf', 'sqrtf', 'sinf', 'cosf',
               'atan2f', 'hypotf', 'powf', 'expf', 'logf')


def float_calls(edges: Path, args, addrs: list[int], names: list[str], by_name: dict[str, int], frames: int) -> None:
    """calls into the float routines (edges whose target is one of their entry points), per calling function"""
    entry = {}
    for n in FLOAT_ENTRY:
        for k in (n, '_' + n):
            if k in by_name:
                entry[by_name[k]] = n
    calls: list[tuple[int, str, int]] = []
    for line in edges.read_text().splitlines():
        f = line.replace(',', '\t').split('\t')
        try:
            a, b, n = int(f[0], 16), int(f[1], 16), int(f[2])
        except (ValueError, IndexError):
            continue
        if b in entry:
            calls.append((a, entry[b], n))
    a2l = Path(str(NM).replace('-nm', '-addr2line'))
    res = subprocess.run([a2l, '-f', '-i', '-e', args.elf], input='\n'.join(hex(a) for a, _, _ in calls),
                         check=True, capture_output=True, text=True).stdout.splitlines()
    # with -i an address may print several (inlined) frames: re-run one at a time for the innermost caller
    per: dict[tuple[str, str], int] = {}
    for a, fn, n in calls:
        r = subprocess.run([a2l, '-f', '-e', args.elf, hex(a)], check=True, capture_output=True, text=True).stdout.splitlines()
        name = r[0] if r else '?'
        loc = r[1].rsplit('/', 1)[-1] if len(r) > 1 else ''
        per[(f'{name} ({loc})', fn)] = per.get((f'{name} ({loc})', fn), 0) + n
    out = args.profile.with_suffix('.callers.tsv')
    tot: dict[str, int] = {}
    for (caller, _), n in per.items():
        tot[caller] = tot.get(caller, 0) + n
    with open(out, 'w') as o:
        o.write('caller\tcalls_per_frame\troutines\n')
        for caller, n in sorted(tot.items(), key=lambda kv: -kv[1]):
            fns = sorted(((fn, m) for (c, fn), m in per.items() if c == caller), key=lambda kv: -kv[1])
            o.write(f'{caller}\t{n / frames:.0f}\t' + ' '.join(f'{fn}:{m / frames:.0f}' for fn, m in fns) + '\n')
    print(f'float calls by caller -> {out}')


if __name__ == '__main__':
    sys.exit(main())
