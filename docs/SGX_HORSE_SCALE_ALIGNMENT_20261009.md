# SGX convoy horse size and alignment correction

The row added in `db59af6` baked the 128×80 source artwork down to
64×40 inside a 64×48 canvas, then drew it at actor Y−88. The existing
horses use actor Y−48. This produced small horses floating above the convoy.
It also left the added horses fixed while the original horses shook vertically.

Both VDCs now use the same palette and full 128×80 source poses. VDC0
horses use the same Y anchor and frame shake, with a 112-pixel horizontal
stagger (half the adapted convoy's 224-pixel spacing). Foreground admission
still precedes the added horses. Each horse has four 32×64 upper cells and
four 32×16 lower cells; scanline reservations are batched by common Y span.

VDC0 reserves pages 28–47 as two full-size pose buffers and reuses the
existing four-slice upload schedule. VDC1 retains all five poses. The smaller
baked poses are removed. Only a completed buffer is referenced by new SAT
entries; the next pose is prepared in the alternate buffer.

The convoy regression checks both planes' full-size cell layout, matching
published Y anchors, intact displayed pose data, all five animation poses,
scanline limits, foreground retention, projectiles, and post-convoy scrolling.

Validation on the accurate headless emulator with hardware sprite limits:

- Both retail builds and their ELF/bank checks pass.
- PCE convoy regression: all three convoys pass.
- SGX convoy regression: all three convoys pass, including full-size pose
  integrity, Y alignment, 112-pixel X stagger, all five poses, foreground and
  hero projectile retention, scanline budgets, and post-convoy scrolling.
- SGX rendering, depth, and cache-pressure checks pass.
- Matching convoy captures were inspected: `build/sgx/herd-1-locked.png`,
  `herd-2-locked-firing.png`, and `herd-3-locked.png`.
- The strict herd-1 firing profile records 299 presentations / 300 VBlanks
  (59.8 fps), one initial firing-pose cache miss, and zero essential
  overflows. Its strict 60 fps assertion fails; the remaining nine 30-frame
  windows each present 30 frames. This is a measured limitation of the
  full-size streamed row, not a sustained 60 fps claim.

No physical-console validation was performed.
