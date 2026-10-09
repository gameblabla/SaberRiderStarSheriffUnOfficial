# SGX sky motion, level startup and horse residency

Follow-up to `sgx_structural_fixes_2026-10-08.md` and the referenced
`codex-session-01a11c1c-9da1-78e1-a842-bc0a0ae2c25b.md`.

The player-based sky behavior described in the initial version of this note
was incorrect and is superseded by
`sgx_camera_sprite_regressions_2026-10-08.md`. That report records the current
implementation and its binary verification.

## Changes

- Platform skies follow the foreground camera with their own Q8 ratios.
  Walking within a fixed camera view leaves the sky fixed; camera catch-up
  advances it even when the player is stationary.
- A later repetition of the panorama provides a positive world coordinate
  across the whole stage. Source columns repeat modulo the panorama width,
  while BAT columns keep advancing continuously across source seams. Ordinary
  seam crossings need one streamed column rather than a full cache refill.
- The sky records for stages 1, 3, 4 and 5 repeat their source maps. The asset
  baker budgets all cyclic 33-column windows, including those straddling the
  last and first source columns. Stage 4's lower band repeats independently.
- Platform startup draws both new sprite tables and loads the complete sky
  window while the display is disabled, then waits for VBlank before fading
  in. Sky display admission requires an initialized window. Full cache
  refills hide VDC1 until the shared SAT VBlank installs the matching scroll
  and actor table, avoiding stale BAT contents and old-scroll exposure.
- All five horse poses load on VDC1 during scene startup. Three use
  `$2800..$45ff`; two use `$6400..$77ff`. Actor allocations protect the latter
  pages throughout the convoys. The reservation ends when the boss begins.
  Starting a convoy changes the horse palette/state without overwriting
  patterns referenced by a displayed actor SAT. VDC0 scenery may use those
  same addresses in its own VRAM.

## Validation

Build and native emulator testing are delegated to GPT-6-Luna, as requested.
See `sgx_camera_sprite_regressions_2026-10-08.md` for the final verification
results and artifact paths.
