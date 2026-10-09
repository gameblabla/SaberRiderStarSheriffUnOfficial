# SGX and shared PCE fixes from sgx_issues_left.txt

## Changes

- All story layouts use the shared `dialog_layout_pce.h` coordinates. Panels, text and portraits move up 16 pixels; portraits start at X=0. The race raster switches before the top border instead of cutting off its first line. Closing a race panel restores its relocated BAT region.
- SGX stampedes have an additional smaller, staggered horse row on VDC0. All five poses are resident in reserved VRAM pages. Its SAT entries use VDC0's scanline admission checks; the original row remains on VDC1. No recurring animation upload is needed for the new row.
- Level 4's lower mountain raster band now contains the source sky behind transparent mountain pixels. A different vertical band on the same VDC cannot show through transparency; that was the cause of the black holes. Stage 4 and 5 camera sweeps also verify BAT cells and pattern contents throughout the level width.
- SGX Game Over reserves the same four lettering palettes as PCE and animates their additive color phases. The other twelve painting palettes remain unchanged.
- Race signed multiplication uses the resident quarter-square table. The assembly byte multiply preserves Y and passes exhaustive native validation of all 65,536 signed input pairs. UI VRAM helpers move to bank 113 to keep the enlarged road bank within its size limit.
- Sprite transfers use longer interrupt-safe bursts when a raster split is inactive. PCE horse warm-up releases unused firing-pose pins when fire is released, allowing idle/run animations to enter the cache after dialogue closes.

The commit also includes the previously uncommitted SGX/PCE tree: archive compression/loading, initialization, frontend, pause, projection, house/moon, audio and cache fixes, with their existing tests and reports. Those earlier reports describe their own validation snapshots; the results below describe this revision.

## Validation

Both retail discs rebuild successfully:

```
make -f Makefile.pce SGX=1 OUT=build/sgx
make -f Makefile.pce OUT=build/pce
```

All 17 commands in `test-sgx` pass against the final SGX image, including the corrected projectile-fixture rerun described below. Command results, the initial fixture failure and individual logs are stored in `build/sgx/final-validation/`. Native checks cover frontend pulse, campaign and ending, motion, houses/moon, rendering, sprite admission/cache pressure, dialogue and race restoration, exact road arithmetic, and late-stage graphics.

The final projectile regression fixture additionally requires continuity from the preceding publication. A shot can spawn after VBlank publishes the SAT but before the debugger samples CPU memory, so the former two-snapshot check mistakenly classified newborn shots as persistently displayed. The corrected fixture retains the persistent-bullet assertions and checks 60 publications.

The SGX herd 1 strict profile records 300 presentations in 300 video frames (60 fps), ten consecutive 30/30 windows, and zero essential sprite refusals with both horse rows active. All three herd fixtures check stable animation patterns and scanline budgets.

The standalone PCE campaign reaches the ending. PCE dialogue restoration passes for scroll phases 0, 5 and 7, restoring 990 cells, font and palette in each case without essential sprite refusals. Software ADPCM checks sample data, bank transitions, looping/reset and both channels. The general PCE `test` target does **not** pass: its first walking fixture uses the removed retail debug-pause selector. This is a fixture limitation, not a passing suite result.

Late-stage checks compare the complete streamed main backgrounds and sky across stages 4 and 5. Stage 6 and 7 are seeded native firing scenes, not manual playthroughs; both stay within each VDC's sprite budget with zero essential refusals. Screenshots and `late-stage-sweep.json` are under `build/sgx`.

## Remaining performance limits

The visual fixes and extra horse row are verified, but sustained 60 fps across every mode is **not achieved**:

| Sample | Measured cadence |
| --- | --- |
| SGX stage 1, six-enemy cold combat fixture | 163/180 presentations, 54.33 fps |
| SGX stage 3, settled combat fixture | 179/180 presentations, 59.67 fps |
| SGX racing road updates | approximately 20 updates/second |
| SGX stage 6 seeded firing scene | 73/180 presentations, 24.33 fps |
| SGX stage 7 seeded firing scene | 177/180 presentations, 59 fps |
| PCE stampede profile | 290/300 presentations, 58 fps; initial cold window drops frames |

The cold dense-combat fixture reaches 30/30 after the initial burst but still fails a strict 60 fps requirement. Racing remains a substantial performance limitation despite the arithmetic changes. Reports distinguish game/presentation cadence from the emulator's 60 Hz video clock. No physical-hardware validation was performed.
