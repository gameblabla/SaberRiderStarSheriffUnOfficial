#!/usr/bin/env python3
"""Regression runs of the headless build (Makefile.headless) for arithmetic changes (the fixed-point core).

  tools/regress.py record DIR [--cases a,b] [--fixed]   run every case, keep its trace (stderr) and draw log (gzip)
                                               in DIR (--fixed: the 16.16 build, make -f Makefile.headless FIXED=1)
  tools/regress.py compare BASE NEW [--tol T]  per case: the first frame whose draws or trace differ by more than T
                                               (numbers compared with tolerance, words exactly), and how far apart

A case is a start level (0: the front end) with an input: the level-1 benchmark script (bench) or random presses
(fuzz: libc rand(), the same sequence each run). The draw log keeps every 4th frame."""
import argparse, gzip, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build/headless/saber_headless')


def bench_script():
    for line in open(os.path.join(ROOT, 'tools/saturn/bench_level1.env')):
        if line.startswith('SABER_SCRIPT='): return line.strip().split('=', 1)[1]
    raise SystemExit('no SABER_SCRIPT in bench_level1.env')


# the power attack (fuzz leaves it out), every ~11 s on top of the benchmark's walk-and-shoot (its presses page the dialogs)
POWER = '120:,' + ','.join(['25:RS,5:R,25:RS,5:RJ'] * 11 + ['3:X'] + ['25:RS,5:R,25:RS,5:RJ'] * 11 + ['3:X']
                           + ['25:RS,5:R,25:RS,5:RJ'] * 11 + ['3:X'] + ['25:RS,5:R,25:RS,5:RJ'] * 20)


def cases():
    """name: (start level, input, frames, extra environment)"""
    out = {'menu_bench': (0, 'bench', 3600, {}), 'menu_fuzz': (0, 'fuzz', 3600, {})}
    for lv in range(1, 8):
        out['s%d_bench' % lv] = (lv, 'bench', 3400, {})
        out['s%d_fuzz' % lv] = (lv, 'fuzz', 3600, {})
    for h in range(4): out['s1_power%d' % h] = (1, POWER, 3000, {'SABER_HERO': str(h)})
    for lv, h in ((3, 1), (4, 2), (5, 3), (7, 0)): out['s%d_power%d' % (lv, h)] = (lv, POWER, 3000, {'SABER_HERO': str(h)})
    # the ends of the platform stages (their bosses), a death and respawn
    out['s1_boss'] = (1, 'bench', 3000, {'SABER_START': '8000', 'SABER_BOSSHP': '12'})
    out['s3_boss'] = (3, 'bench', 3000, {'SABER_START': '6700', 'SABER_BOSSHP': '12'})
    out['s4_boss'] = (4, 'bench', 3600, {'SABER_START': '6200', 'SABER_BOSSHP': '12'})
    out['s5_dark'] = (5, 'bench', 3000, {'SABER_START': '6500', 'SABER_DARK': '1', 'SABER_DARKHP': '12'})
    out['s5_boss'] = (5, 'bench', 3000, {'SABER_START': '6500', 'SABER_BOSSHP': '12'})
    out['s1_kill'] = (1, 'bench', 1500, {'SABER_KILL': '400'})
    return out


def record(d, only, exe):
    os.makedirs(d, exist_ok=True)
    for name, (lv, inp, frames, extra) in cases().items():
        if only and name not in only: continue
        env = dict(os.environ, SABER_ASSETS=os.path.join(ROOT, 'assets'), SABER_FRAMES=str(frames), SABER_TRACE='3',
                   SABER_DRAWLOG=os.path.join(d, name + '.draw'), SABER_DRAWLOG_EVERY='4')
        env.pop('SABER_PERF', None)
        env.update(extra)
        if inp == 'bench': env['SABER_SCRIPT'] = bench_script()
        elif inp == 'fuzz': env['SABER_FUZZ'] = '1'
        else: env['SABER_SCRIPT'] = inp
        with open(os.path.join(d, name + '.trace'), 'w') as err:
            r = subprocess.run([exe, os.path.join(ROOT, 'SaberRider/data'), str(lv)], env=env, stdout=subprocess.DEVNULL,
                               stderr=err, cwd=ROOT)
        subprocess.run(['gzip', '-f', os.path.join(d, name + '.draw')])
        print('%-12s exit %d' % (name, r.returncode), flush=True)


NUM = re.compile(r'^-?\d+(\.\d+)?(e[-+]?\d+)?$')


def frames(path):
    """(frame number, lines) for a draw log, or (line index, [line]) for a trace"""
    if path.endswith('.gz'):
        cur, n = [], 0
        with gzip.open(path, 'rt') as f:
            for line in f:
                if line.startswith('F '): yield int(line[2:]), cur; cur = []
                else: cur.append(line.rstrip('\n'))
        if cur: yield -1, cur
    else:
        with open(path) as f:
            for i, line in enumerate(f): yield i, [line.rstrip('\n')]


def tokens(line): return re.split(r'[ ,=:/]+', line)


def diff_line(a, b, tol):
    """None if the same within tol, else the largest numeric difference (inf for a word or shape difference)"""
    if a == b: return None
    ta, tb = tokens(a), tokens(b)
    if len(ta) != len(tb): return float('inf')
    worst = 0.0
    for x, y in zip(ta, tb):
        if x == y: continue
        if NUM.match(x) and NUM.match(y): worst = max(worst, abs(float(x) - float(y)))
        else: return float('inf')
    return worst if worst > tol else None


def compare(base, new, tol, only):
    bad = 0
    for name in cases():
        if only and name not in only: continue
        for ext, what in (('.trace', 'trace'), ('.draw.gz', 'draw')):
            pa, pb = os.path.join(base, name + ext), os.path.join(new, name + ext)
            if not (os.path.exists(pa) and os.path.exists(pb)): continue
            first, nbad, worst, total = None, 0, 0.0, 0
            for (fa, la), (fb, lb) in zip(frames(pa), frames(pb)):
                total += 1
                d = float('inf') if len(la) != len(lb) else None
                if d is None:
                    for x, y in zip(la, lb):
                        e = diff_line(x, y, tol)
                        if e is not None: d = e if d is None else max(d, e)
                if d is not None:
                    nbad += 1; worst = max(worst, d)
                    if first is None: first = (fa, la, lb)
            if first is None: print('%-12s %-5s same (%d)' % (name, what, total)); continue
            bad += 1
            fa, la, lb = first
            print('%-12s %-5s %d/%d differ, first at %d, worst %s' % (name, what, nbad, total, fa, 'shape' if worst == float('inf') else '%.3f' % worst))
            shown = 0
            for i in range(max(len(la), len(lb))):
                x = la[i] if i < len(la) else '<none>'; y = lb[i] if i < len(lb) else '<none>'
                if x != y and diff_line(x, y, tol) is not None:
                    print('    - ' + x[:200]); print('    + ' + y[:200]); shown += 1
                    if shown == 3: break
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['record', 'compare'])
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--tol', type=float, default=0.01)
    ap.add_argument('--cases', default='')
    ap.add_argument('--fixed', action='store_true')
    ap.add_argument('--exe', default='')
    a = ap.parse_args()
    only = set(filter(None, a.cases.split(',')))
    if a.cmd == 'record': record(a.dirs[0], only, a.exe or (EXE.replace('/headless/', '/headless-fx/') if a.fixed else EXE))
    else: sys.exit(1 if compare(a.dirs[0], a.dirs[1], a.tol, only) else 0)


if __name__ == '__main__':
    main()
