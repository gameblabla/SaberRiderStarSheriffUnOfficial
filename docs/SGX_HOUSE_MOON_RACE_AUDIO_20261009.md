# House graphics, red moon and race boss music

The work continues the SGX session
`codex-session-01a120b7-f6da-7b81-9a6a-256f0595a269.md` and preserves its
uncommitted changes. The supplied house screenshot and Mednafen state identify
rotating fan props pasted into the hotel facade by `scenery_background()`.

## House correction after visual review

The initial fan-omission fix below was insufficient: the doors/boards in
other prop frames still overwrote both buildings. `scenery_background()` now
clips every base prop against the opaque tilemap layers after
`MidBGHyperjpr` and before `PlayerSprites`. This restores source depth for
both the saloon and hotel while retaining fan pixels in the actual openings.
The hotel's complete unquantized crop at `(7100,0)-(7356,224)` is pixel-identical
to the supplied `/tmp/house-script-fixed.png` reference.

The house test now independently renders the later terrain masks, checks that
all protected building pixels survive, checks exposed pixels from both fan
frames, captures both buildings, and compares the visible native VDC0 BAT and
pattern data against the rebuilt stage archive. The earlier omission test
was inadequate because equality with an unfilled source opening did not prove
that the complete facade looked correct.

The correction rebuilt both `build/sgx/saber_rider.cue` and
`build/pce/saber_rider.cue`. The updated `test_sgx_house_moon.py` and
`test_sgx_rendering.py` passed. Both native building captures were visually
reviewed: `build/sgx/saloon-disc-fixed.png` and
`build/sgx/hotel-disc-fixed.png`. Python compilation and `git diff --check`
passed. The full SGX suite was not repeated for this asset-depth correction;
the full-suite result below belongs to the earlier revision.

## Original changes (house treatment superseded above)

- `tools/pce/build_assets.py` omits prop types 17 and 18 from the flattened
  background. Their actor records remain available on the scenery plane. The
  source rectangles are `(6544,80)-(6592,112)` and
  `(7168,80)-(7216,112)`. Other prop fills, including security cameras, retain
  their previous treatment.
- Stage 3 builds its moon into an inactive VDC1 BAT and pattern buffer. The
  VBlank callback publishes the completed page with the frozen scene scroll.
  VDC1 uses a 64 by 64 BAT: pages at VRAM words `$0000` and `$0800`, the sky
  cache at `$1000-$2fff`, and moon buffers at `$3000-$3a8f` and
  `$3a90-$451f`, below sprite patterns at `$4800`.
- A VDC VRAM copy is pending until blanking, with BSY still clear. Waiting only
  for BSY to clear allowed the later DMA to overwrite already-restored moon
  cells and preserve old crescents. The streamer now waits for DMA start or
  its VBlank, then drains BSY before editing the inactive BAT. Interrupts remain
  enabled while waiting. The moon keeps its independent parallax and remains
  behind terrain and actors.
- Race boss music is requested from the active boss phase after dialogue,
  rather than requiring a pending transition edge. A per-race latch prevents
  repeated seeks. The ordinary boss transition already produced track 17 in
  the rebuilt baseline; the original reported silence was not reproduced.
  Verification also covers restoring the boss phase without a pending edge.

## Regression checks

`test_sgx_house_moon.py` compares both fan rectangles with the unmodified
source layer composition, records 180 consecutive moving-camera frames, and
checks exact sky/moon BAT and pattern data at camera positions that exposed
old crescents. It exercises both background pages and writes
`build/sgx/house-moon-verification.json`. Reviewed captures include
`house-props-fixed.png` and `moon-moving-{0,60,150}.png`.

`test_sgx_race.py` checks physical audio output and the selected track after
normal boss dialogue and restored boss phase. It writes
`race-boss-transition.wav`, `race-boss-restored-phase.wav`, and the existing
race verification report.

The two existing sky validators now inspect the selected BAT page.
`test_sgx_house_moon.py` is part of `make -f Makefile.pce test-sgx`.

The isolated CD-DA test fixture now sets hardware registers with actual CPU
stores: the emulator's high-level debugger pokes bypass I/O handlers. It also
waits for a native audio call to return before checking stop postconditions;
a cancelled CD seek can span multiple VBlanks.

## Verification status

Both SGX and standard-PCE disc builds passed. The complete
`make -f Makefile.pce test-sgx` target passed, including all twelve test scripts:
campaign, front end, motion, rendering, house/moon, boss/projectile regressions,
reported issues, race, cache pressure, scenery depth, combat profile and herd.

The standard-PCE `test_cdda.py` passed hardware control-bit preservation,
fade clearing, audible playback at each enabled music setting, replacement of
a pending cue with track 17, cancelled-seek stopping, one-shot completion,
repeat restart and music-off silence. Its steady-state silence check allows
the mixer's short stop filter tail to drain first.

Both native SGX boss recordings selected logical track 17 and produced audio
(RMS approximately 2585 and 2628). The house/moon check exercised both BAT
pages, verified exact sky data before/after motion and found no disappearing
moon in its 180-frame capture. Captures were visually reviewed; the former
extra crescent is absent. Python compilation and `git diff --check` passed.

Rebuilt discs: `build/sgx/saber_rider.cue` and
`build/pce/saber_rider.cue`. Keep their sibling ISO and music files together.

The additional standard-PCE `test_graphics_stability.py` run failed on
`essential_overflow=19`, with refused sprite 188 (`hero0_idle4`) at camera 5800.
This broader sprite-admission failure remains unresolved. No claim is made
that it predates these changes without a matching baseline run.

These checks use the accurate-core headless emulator with native code and
seeded scenarios. They are not a manual campaign playthrough or physical
hardware validation. Old emulator save states contain the old code and VRAM;
verify the rebuilt disc from a fresh boot or an in-game save.
