# SGX forest and Ramrod fixes — 2026-10-10

Input: `sgx_issues_last3.txt` and `codex-session-01a12369-b055-7881-a617-acf8c3d7f55b.md`.
The user confirmed that the reported level 5 is Ramrod's robot battle,
called stage 6 by the runtime.

## Changes

- Forest (stage 4): retain the continuous sky/mountain panorama and stream it
  at quarter camera speed (Q8 64 rather than 256). The ground and foreground
  continue following their existing camera paths.
- Ramrod dialogue: use screen columns on BG0 for panel cells, text, and sprite
  corners. Clear the robot compositor and select BAT half zero before opening
  a story. Previously the panel and text inherited the panorama's scroll,
  while BG0 displayed an independent BAT half.
- Ramrod sprite publication: upload the VDC1 SAT before restoring VDC0's count.
  The upload retires entries according to `sat_count`; using VDC0's smaller
  count cleared valid distant sprites, including full-size robot cells.
- Ramrod scaling: the nearest large robot uses BG0; the next large robot uses
  VDC1's double-buffered 32×32 sprite cells at its selected baked size. Large
  sprite uploads target VDC1 directly. This provides two large robots, versus
  the PCE renderer's one. A third close robot retains the smaller sprite ladder.
- Ramrod depth: split sprites at the nearest BG robot's actual depth, keep
  projectiles on their depth's plane, and sort props by real distance. Rear
  sprites no longer retry above a nearer robot on VDC0. Both sprite planes
  retain their independent scanline and SAT limits.

## Build and evidence

Retail SGX disc with the existing independent level selector:

```sh
make -f Makefile.pce SGX=1 RETAIL=1 DEBUG=0 LEVEL_SELECT=1 OUT=build/debug-sgx all
python3 tools/pce/test_sgx_last3.py --out build/debug-sgx
python3 tools/pce/test_sgx_last2.py --out build/debug-sgx --only-stage4
```

Disc: `build/debug-sgx/saber_rider.cue`.
The focused last3 check covers visible forest BAT/patterns at several camera
positions, dialogue screen cells and glyphs, two overlapping large robots,
VDC1 sprite patterns against the baked archive, code-bank integrity, and a late
story followed by compositor restoration. The last2 check covers native forest
walking across the BAT wrap and power restoration at five camera positions.
Both focused checks passed. The two-robot fixture retained all eight VDC1
32×32 cells and matched 4096 uploaded pattern bytes to the archive. Forest
samples included camera 2054 with sky offset 514, crossing the sky BAT wrap.
Captures and JSON reports are written beside the disc.

The PCE application was also compiled and linked with its existing baked assets
to check compatibility of shared code. This is not a regenerated PCE disc.
Evidence is from debugger-seeded accurate-core emulator scenarios, with hardware
sprite limits enabled; no physical console or full campaign playthrough was run.
