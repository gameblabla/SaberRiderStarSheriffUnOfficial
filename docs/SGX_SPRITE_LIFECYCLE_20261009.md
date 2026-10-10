# SGX foreground, horse animation and pause fixes

Starting revision: `fb8199a`.

The supplied `saber_rider.29fa56a6477076d9475483f9199352c2-missing_sprite_foreground.mc0`
contains stage 1 telemetry for April at player (8609, 177), camera X=8489.
Loading this older state preserves its older application RAM, so verification
uses the rebuilt native disc at matching positions and a preceding camera sweep.

## Causes and repairs

- Foreground chunks enter the retained list at canonical X=96. Accumulating
  their temporary scanline reservations incorrectly makes chunks at different
  world positions compete during retention. SGX now clears the temporary SAT
  and admission budget for each chunk; the actual draw still admits pieces at
  their screen positions.
- The VDC0 pattern allocator excluded pages 8–13 for a hero drawn on VDC1.
  With pages 28–47 reserved for two horse poses, dense foreground exhausted its
  remaining pages. VDC0 foreground can now use pages 8–13; existing page-owner
  and displayed-generation checks still protect live sprites. HUD pages 0–7
  and horse pose storage remain reserved.
- A missed animation tick can switch horse poses during phases 1–3. Prefetching
  immediately then overwrites the old pose while its SAT is still displayed.
  The shared streamer waits for pending publication and defers prefetch after
  every pose switch until a subsequent draw.
- Pause hid VDC1 but retained the added VDC0 horse row. The temporary stage-1
  pause SAT now hides that row while preserving the original snapshot for
  unpause. VDC1 hiding is also atomic with clearing pending publication, so an
  interrupt cannot reenable sprites midway through the operation.

## Regression coverage

Foreground checks now compare all source descriptor pieces with the retained
list, in addition to comparing retained pieces with the hardware SAT. This
detects chunks missing before retention, which the previous assertion missed.
The dense camera sweep exercises incremental window changes before the saved
camera position. Horse checks preserve the previous displayed buffer on every
pose switch, including skipped ticks. Pause checks require both rows to hide
and restore while retaining HUD and foreground entries.

The isolated, IRQ-masked horse helper test explicitly acknowledges its synthetic
publication before each stream call; active-IRQ convoy tests exercise normal
publication separately.

## Validation

Both final retail discs build and pass their ELF/bank checks:

```
make -f Makefile.pce SGX=1 OUT=build/sgx all
make -f Makefile.pce OUT=build/pce all
```

Luna at xhigh runs the binary regression checks on the accurate headless
Mednafen core with hardware sprite limits enabled.

- `test_sgx_rendering.py`: passes source-piece coverage and hardware SAT
  checks for stages 1, 3, 4 and 5, the supplied-state camera fixtures, the
  eleven-position dense camera sweep, and projectile/death presentation.
- `test_sgx_foreground_walk.py`: the pre-fix disc fails all eleven positions
  from camera 8416 through 8489; the final disc retains every expected source
  piece at every position. At 8489 all thirteen source pieces are retained
  and rendered. Before/after stage archives have identical SHA-256 hashes.
- `test_herd.py --sgx`: all three convoys pass pose integrity, matching row
  baselines, hardware admission, foreground and projectile retention. The
  first convoy's pause check observes zero live VDC0 horse entries and a
  disabled VDC1 sprite-enable bit; both rows return on unpause and retained
  VDC0 HUD/foreground entries are preserved.
- The pre-fix pause check fails after the PAUSE SAT has actually published:
  four live VDC0 horse cells remain. The check waits for the label and DMA,
  rather than treating VDC1's earlier hide flag as proof of pause completion.
- `test_herd_render.py --out build/pce`: passes 160 SAT/occupancy cases,
  32 HUD cases, five animation poses and 32 stream ticks, including skipped
  ticks. The starting SGX image with SGX disabled fails this same helper at
  tick 26: the previously displayed horse buffer changes before publication.
- `test_herd.py --out build/pce`: all three native convoy scenarios pass.

The isolated helper derives bank selection from ELF symbols, verifies that
native calls return to their sentinel, and disables palette presentation while
hardware IRQs are masked. Otherwise a full palette queue can wait forever for
the VBlank that this fixture deliberately disables.

The isolated pre-fix image is `build/sgx-before/saber_rider.cue`, built from the
starting revision's affected runtime files. Matching scroll captures and JSON
reports are in each build directory: `foreground-camera8489-walk.png` and
`foreground-walk-verification.json`. Direct jumps to the final camera produce
identical images; the preceding dense window is required to reproduce the
permanently omitted chunks.
