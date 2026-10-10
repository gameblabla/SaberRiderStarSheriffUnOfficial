# SGX arena save-state fixes — 2026-10-10

Checkpoint before edits: `859d0d6` (pending PCE race and SGX work).
Inputs: `save_sgx/*bottomleft_spriteglitch.mc0`,
`save_sgx/*terrain_glitch.mc0`, and the requested session transcript.

## Observations and changes

The saved VDC0 BAT contains eight invalid cells in column zero at rows 24–31
in the sprite fixture. The terrain fixture has eighteen invalid cells,
including cell zero. These are background cells, not additional SAT entries.
The old blank cell referenced character zero, whose 32 pattern bytes are also
BAT cells 0–15. Corruption of the first BAT cell consequently changes the blank
character everywhere and produces the repeated horizontal bands.

The arena now fills its BAT and erased robot regions with character `$3ff`,
whose zero pattern lives at VRAM word `$3ff0`. This is the end of the unused
VDC0 big-sprite buffer on SGX: large sprite robots use VDC1, and the second arm
buffer begins at `$4000`. The blank pattern is separate from the BAT, the BG
robot patterns, sprite cache, HUD/bolts and SAT sources.

Contiguous VDC0 Arcade Card uploads now explicitly clear CR bits 11–12 before
programming their destination. They no longer depend on a preceding BAT
column operation having restored the one-word increment. Arena clear/map
writes also use the correct `$1800` increment mask (the old `$3000` mask left
bit 11 intact). A native upload probe using the pre-fix code restored from the
sprite save failed byte-for-byte contiguity with a 64-word increment; a 2304-byte
sprite block also wrote into the BAT. The fixed
probe checks all four hardware increments and requires the BAT to remain
untouched for a block of that size. The exact earlier operation that left the saved invalid cells was
not captured; selecting the upload stride is additional protection against that
class of overwrite, not a claim to have traced its original instruction.

## Verification

Build:

```
make -f Makefile.pce SGX=1 RETAIL=1 DEBUG=0 LEVEL_SELECT=1 OUT=build/debug-sgx all
python3 tools/pce/test_sgx_arena_saves.py --out build/debug-sgx
python3 tools/pce/test_sgx_last3.py --out build/debug-sgx
```

All three passed. The new test extracts the reported gameplay data from both
original saves, enters stage 6 on the rebuilt disc, and replays those values
with fresh code/cache/display state. It checks every BAT cell and the protected
blank pattern across 180 frames per fixture, then checks pause. It also runs a
native CPU upload probe for all four CR increments. The last3 check retains
forest parallax, dialogue restoration and the two large robots.

The headless emulator cannot directly resume the original saves: its RAM
layout is 8 KiB base RAM and 384 KiB extended system RAM, versus standard SGX
32 KiB base RAM and 192 KiB system RAM in the supplied files. Temporary converted
copies allowed original-code diagnosis, but the committed regression uses
parsed gameplay values on the fresh disc rather than pretending a direct
old-state load runs the newly built code. Original saves are untouched.

Disc: `build/debug-sgx/saber_rider.cue`.
Captures: `build/debug-sgx/arena-{sprite,terrain}-after.png` and matching pause
captures. Report: `build/debug-sgx/sgx-arena-saves-verification.json`.
The broader SGX run passed its arithmetic and seeded campaign checks. Three
stale fixtures were updated to match the checkpoint: frontend options move up
one row when LEVEL_SELECT is enabled; the stage-4 motion fixture now
requires quarter-speed parallax and selects a camera that crosses a BAT edge;
and the race-dialogue check uses the checkpoint's split low/high quarter-square
tables and reciprocal-table offset 1024.
The PCE variants of both changed C files passed warnings-as-errors syntax checks.
This is accurate-core emulator evidence, not a physical console test.

All 23 checks listed under `Makefile.pce:test-sgx` passed across the resumed
runs after those three fixture corrections. Logs are under
`artifacts/sgx-save-fixes/full-sgx-test*.log`. This includes the seeded campaign,
frontend, sky/motion/rendering, house/moon, CD-DA lead-out, prior regressions,
race, cache pressure, depth, combat performance, herd/exit, race math, baked
curves, road refresh, late-stage sweep, last2 and last3 checks. The road refresh
fixture presented 29.75 generations per second against its 25 minimum.
The new save-data check and four 2304-byte upload-stride probes also passed.

For a playtest, start a fresh run on the rebuilt disc (its level selector can
start stage 6). Loading an original save restores its old loaded code and old
VRAM, so it is not a valid way to test the new implementation directly.
