# SGX added horse left-edge exit

The shared enemy update retired horse actors at `actor X < camera - 80`.
The full-size added horse starts at `actor X - camera + 40`, so it vanished
at X = -41 while most of its 128-pixel width remained onscreen.

SGX gameplay now keeps horse actors through `camera - 167`. The added horse
retains its final column at X = -127 and is fully culled at X = -128.
Existing per-column renderer culling already discards each 32-pixel column
only when it is fully offscreen. Other actor types and the PCE cutoff retain
their existing behavior.

`test_sgx_herd_exit.py` calls the compiled actor update and renderer across
138 exit positions, checks all surviving column coordinates, and verifies
that the convoy remains active until the added horse exits. It fails against
the previous build at X = -41 and passes with the fix. It is included in
`make -f Makefile.pce test-sgx`.

Both retail builds and their ELF/bank checks pass. All three native convoy
regressions pass in both PCE and SGX. Validation uses the accurate headless
emulator with hardware sprite limits; physical hardware was not tested.

## Shared VDC geometry and rendering

Both VDC passes now call `herd_prepare` and `herd_render`. They share the
72-pixel X anchor, column clipping, scanline admission, and SAT emission.
`herd_geometry.h` defines the width, anchor, convoy spacing, and half-spacing
stagger used to position the added horses between the original horses.
The exit threshold is derived from this geometry rather than a separate
hardcoded coordinate. Pose storage remains specific to each VDC, and each
pass applies world shake at its existing point in the rendering sequence.

The native exit test also compares two horses on each VDC at 387 normalized
screen positions across both edges. Coordinates, cell sizes, palettes, and
visibility agree with each other and an independent per-column reference.

Both retail rebuilds, all PCE/SGX convoys, and the 138-position exit sweep
pass after the refactor. Additional PCE `test_herd_render.py` and
`test_herd_visibility.py` checks fail at the same assertions on an isolated
build of the previous commit: randomized case `(0, 4)` and fixed HUD
coordinates respectively. Those existing failures remain outside this fix.
