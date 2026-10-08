# SGX performance optimization report — 2026-10-09

Work continued from `codex-session-01a11d34-2db2-7d80-b52c-3c2bb989ee10.md` and commit `608b625`. Optimization was stopped at the user's request. The requested sustained 60 fps target has **not** been achieved.

## Measured result

The same seeded stage-1 walking/firing combat fixture improved from **37.0 fps to 53.67 fps**, about **45% more presented frames per second**. This measures new SAT presentations across 180 emulated video frames, with accurate hardware sprite limits enabled.

| Measurement | Latest build |
| --- | ---: |
| New presentations / video frames | 161 / 180 |
| Average fps, using the fixture's nominal 60 Hz calculation | 53.67 |
| Missed video frames | 19 |
| Active enemies | 3–6 |
| Essential sprite overflows | 0 |
| Displayed actor palette checks | 262 |
| Native 32-wide pattern checks | 82 |
| Sprite upload bytes, modulo the 16-bit telemetry counter | 49,056 |

Presentations in successive 30-frame intervals were `13, 30, 29, 30, 29, 30`. The last 150 frames presented 148 generations, equivalent to 59.2 fps. The first interval remains substantially slower; later intervals also miss two frames. Those misses remain part of the reported result. The trace shows gameplay allocation, upload, rendering and simulation work during the slowdown, rather than a paused game. It does not establish a single cause for every miss.

Evidence: `build/sgx/combat-heavy.json`, `combat-heavy-trace.json`, `combat-heavy-first30-cycles.txt`, `combat-heavy-cycles.txt`, and `combat-heavy.png`.

## Implemented optimizations

- **Faster collision and physics:** moved collision/physics and allocation code into resident physical bank `$81`; added an assembly cache-hit path for collision cells; enabled inlining of fixed-point position updates. Stationary grounded enemy physics keeps the existing fast path.
- **SGX zero-page allocation:** enabled up to 96 bytes of compiler automatic zero-page allocation for SGX. Added a linker guard preventing overlap with the audio zero-page reservation. Standard PCE retains its previous zero-page setting.
- **Cheaper scanline budget selection:** replaced a runtime operand-patching lookup loop with assembler-generated direct stores. SGX platform stages now use conservative eight-line admission bands; other stages keep exact scanline admission.
- **Better sprite cache use:** marked cache slots used only after a successful visible emission, so refused/offscreen objects do not unnecessarily pin patterns. Added admission probes before uploading cache misses. Reused otherwise available VDC0/VDC1 pages while retaining hero, HUD, SAT and horse reservations. Corrected the allocator's slot sentinel when expanding the SGX descriptor range.
- **Independent palette and pattern ownership:** actor poses can share a palette while retaining separate pattern cache slots. SGX assets share palettes by character family and deduplicate identical palette records. Matching palette identities avoid repeated uploads; foreground/HUD palette identities also avoid redundant uploads when already cached. Family palettes are jointly quantized, so this is an asset-color change as well as a runtime optimization.
- **Native 32×16 actor pieces:** paired suitable adjacent patterns into hardware 32-wide sprites. Pattern bytes and upload counts remain unchanged; the partner descriptor is skipped. Updated flipping, clipping, scanline admission and rollback to account for both hardware scanline units.
- **Direct hero rendering on VDC1:** the platform hero is emitted directly on its final plane, avoiding the usual initial VDC0 emission and cache copy. The story path uses the same approach.
- **Smaller SAT housekeeping:** clear and upload retired entries relative to each alternating VRAM SAT source, avoiding repeated clearing of the entire 64-entry tail.
- **Cheaper encounter handling:** cache convoy trigger indices and skip exhausted triggers earlier, retaining a fallback for larger convoy lists.
- **Overlay packaging and checks:** moved the arena's virtual bank formerly numbered 129 to 145 to free physical `$81`, updated arena image packaging, and updated ELF placement checks to use the actual build flags.

## Validation and delivery

The latest SGX retail build completed successfully, including linker/ELF checks and disc generation. The final combat fixture completed successfully with the palette and native-pattern assertions above. `git diff --check` passed.

Disc entry point: `build/sgx/saber_rider.cue`.

The fixture now supports `--require-60`, which requires exactly one new presentation on every measured frame and no essential overflow. The current measurement does not satisfy that requirement. The ordinary fixture still uses its existing lower performance floor; its successful exit is not evidence of reaching 60 fps.

The full SGX regression suite and a fresh standard-PCE build were **not** rerun against the final changes before the user requested an immediate finish. Full campaign verification is particularly relevant to the arena bank relocation; rendering, herd and cache-pressure checks are relevant to the cache, palettes and band-admission changes. Emulator measurements are not physical-console measurements.

Changes are left uncommitted in the working tree. Existing unrelated untracked files were preserved.
