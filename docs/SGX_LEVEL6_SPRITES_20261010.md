# SGX level 6 sprite retirement and cactus growth

The supplied earlier-build save,
`saber_rider.1c529cf37194ece3e4c0a261e69c9420_level6_brief_spriteglitching_sgx.mc0`,
contains detached sprite fragments and active entries in VDC0's unused SAT
tail (including entries 46, 48 and 62). The instruction that originally wrote
those entries was not captured. Loading the whole save would restore its old
code, so verification replays its 570-byte arena gameplay state on a rebuilt
disc, retaining fresh cache, code and compositor state.

Level 6 now clears and transfers all 64 entries of both alternating SAT sources
on both VDCs. Count-based retirement elsewhere remains unchanged. This prevents
untracked tail entries from reappearing as the sources alternate. A native
regression writes a ghost into entry 63 of all four VRAM SAT sources: before
the fix, the ghost becomes visible on VDC0; after the fix, both hardware SATs
and all four source tails are clear.

The SGX cactus had five baked sizes capped at 40 pixels tall. Its new ladder
keeps five IDs but extends the near size to 101 pixels (44 pixels wide), within
the ordinary sprite cache's 32-pattern limit. It uses its own distance table;
rocks and the standard PCE cactus keep their existing ladders. A native fixture
at distances 1200, 600, 300, 160 and 80 selects heights 15, 29, 55, 101 and 101.
The old heights were 12, 27, 40, 40 and 40.

The arena background bake also now uses the runtime's 512-character budget on
SGX, including the column safety margin. The generated panorama peaks at 474
unique characters in a 33-column window, below its 476-character bake limit.

Verification uses the accurate headless SGX core with hardware sprite limits:

- `make -f Makefile.pce SGX=1 all` (including ELF bank checks).
- `test_sgx_arena_sprites.py --out build/sgx --save <supplied save>`: 300 frames
  of byte-for-byte rear robot pattern checks during punches, four poisoned SAT
  tails, five cactus distances, and 360 frames replaying the supplied gameplay.
- `test_sgx_last3.py --out build/sgx`: two full-size robots and late arena
  dialogue restoration.
- `test_sgx_arena_saves.py --out build/sgx`: earlier sprite/terrain gameplay
  fixtures, pause restoration, and four native upload-stride probes.
- `test_sgx_late_stages.py --out build/sgx`: platform BAT/pattern sweeps and
  stages 6/7 with hardware sprite limits. Stage 6 presented 64 generations
  in 180 video frames (21.33 fps), with no additional essential refusals.
- The three changed C files passed non-SGX warnings-as-errors syntax checks.
- `test_campaign.py --out build/sgx --sgx`: the seeded campaign, ending,
  restart, game over and hero-selection scenarios passed on the final image.

Disc: `build/sgx/saber_rider.cue`. Captures and the arena report are in
`build/sgx/arena-*.png` and `build/sgx/arena-sprites-verification.json`.
This is seeded emulator verification, not a physical-console playthrough.
