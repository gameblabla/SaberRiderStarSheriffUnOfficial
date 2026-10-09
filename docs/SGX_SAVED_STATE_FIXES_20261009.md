# SGX race music and stage 3 moon restoration

Reports: `saber_rider.d21627b6e1e65181e5635dc58392b089_level2phase2_nocddamusicbossplaying.mc0`
and `saber_rider.d21627b6e1e65181e5635dc58392b089_level3_corrupt_graphics.mc0`.

The race state contains boss phase 4, `boss_music_started=1`, CD command `$DE`,
three received bytes, three exhausted attempts and `pce_music_status=$FD`.
The final song uses the disc lead-out as its ending MSF. The driver required
four response bytes even though only the first three contain the MSF. Accept
three or four bytes, rejecting incomplete or oversized responses. The earlier
boss-phase latch correction cannot repair a rejected directory response.

The graphics state has camera 4074, with a stray decorated tile just to the
right of the moon. The previous double-buffer implementation copied the
already decorated displayed BAT and restored old moon cells from VRAM readback.
Its VBlank/BSY wait did not establish DMA completion in every timing case.
Replace this with a 4096-byte undecorated BAT shadow at Arcade RAM `$1FA000`.
The sky cache writes canonical cells to this shadow; each new moon page uploads
the shadow before adding its current moon cells and patterns. Neither decorated
VRAM nor DMA scheduling is used as the source of restoration. This storage is
above the sky directory, reference and held-slot tables and below `$200000`.

## Regression evidence

`test_cdda_leadout.py` invokes the compiled completion handler with three-byte
and padded four-byte directory replies, including exhausted retry state, and
checks active playback and captured audio. The three-byte fixture fails before
the fix with “lead-out reply rejected.”

`test_sgx_moon_restore.py` seeds the reported camera, inserts a stray palette-14
cell at world column 51, row 7, and checks the next native page against the
stage archive. The earlier implementation fails with palette 14 instead of 0
at that cell. It also validates both pages across forward/backward movement,
pixel phases, and larger camera jumps. Both scripts
are included in `make -f Makefile.pce test-sgx`.

The supplied states come from a different Mednafen version. The headless core
reports RAM-size mismatches on direct loading (BaseRAM 32768 versus 8192,
SysCardRAM 196608 versus 393216, and ADPCM length 16 versus 32 bits). Saved RAM
was inspected separately and temporarily mapped to the appropriate CPU banks
for diagnosis. Verification uses a fresh rebuilt disc and native seeded
fixtures rather than claiming a fully compatible old-state replay. Old states
also restore old executable RAM, so load the rebuilt disc from a fresh boot.

These are native emulator tests, not physical hardware validation.

## Completed validation

- SGX and standard PCE disc builds passed, including linker/ELF checks.
- Both new regressions failed on the pre-fix build and passed on the fixed
  build. Three-/four-byte audio captures have RMS approximately 2519/2587.
  The moon check validates 990 native sky cells at each of 19 camera positions.
- SGX campaign, frontend, motion, rendering, house, house/moon, reported-issue,
  race, cache-pressure, depth, combat-profile and herd checks passed. The native
  race transition and restored boss phase both selected logical track 17 and
  produced audio. The rebuilt moon capture was visually inspected.
- The first `test-sgx` run stopped in `test_sgx_regressions.py` at a stage-1
  sustained-fire assertion (sample 55: two persistent hero-bullet candidates,
  one published entry). The same script passed on a pre-fix comparison build
  and on the fixed build rerun, with added failure diagnostics. The diagnostics
  did not alter the fixture or assertions. Remaining scripts were run directly
  and passed. This is not represented as a clean full-target pass.
- Standard-PCE `test_cdda_leadout.py` passed. Its broader `test_cdda.py` failed
  the IFU IRQ-mask preservation assertion at line 77. No assertion is made
  that this failure predates the change without a corresponding baseline run.
- Python compilation and working-tree/staged whitespace checks passed.

Discs: `build/sgx/saber_rider.cue` and `build/pce/saber_rider.cue`, with their
sibling ISO/music files. The commit includes the moon asset/IRQ changes and
supporting overlay relocation needed by this repair. Unrelated pre-existing
worktree edits remain uncommitted.
