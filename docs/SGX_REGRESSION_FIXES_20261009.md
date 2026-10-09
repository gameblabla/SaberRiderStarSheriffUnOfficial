# SGX regressions reported on 2026-10-09

The changes address `sgx_regressions_from_prior_git_commit.txt`. Build the retail disc with `make -f Makefile.pce SGX=1 OUT=build/sgx`; run the native suite with `make -f Makefile.pce test-sgx`.

## Rendering and initialization

- Correct the SDK's reversed initialized-zero-page transfer. It corrupted both title-menu offsets and the racer size ladder. The six racer widths now initialize to 16, 24, 32, 40, 52 and 64 pixels. The title explicitly spells OPTIONS.
- Normalize promoted zero-page pointers before Arcade Card block reads and dialogue rendering. This restores boss descriptors and visible dialogue glyphs. Keep the race's panel-column destination in full RAM so its close operation restores the road BAT correctly.
- Restore herd shake to both backgrounds and world sprites while keeping the foreground SAT fixed. Three native stampedes pass at 60 fps, with background offsets 0–3 and foreground offset zero.
- Put the red moon into the sky background plane, below terrain, actors, scenery, HUD and dialogue. Preserve sky BAT cells outside its visible tiles and match the native sky color in transparent edge pixels. Eight pixel phases retain its independent parallax.
- Use exact scanlines when admitting boss hull pieces; coarse bands falsely combined adjacent hull rows. Native tests check all visible parts, white hit flashes, restored colors and valid projectile patterns.
- Draw PAUSE using the existing HUD palette rather than replacing it with white. Native pause/unpause preserves that palette byte for byte.
- Apply 15% ordered dithering to the title and none to Game Over. Selection has 72 counter-rotating phases and immutable backdrop/portrait palettes.

## Loading

Sector-aligned ZX02 blocks are read in batches through ADPCM RAM and decoded directly into Arcade Card RAM. The decoder is adapted from the local DMSC implementation; its MIT notice is included. Every compressed block has an independent host round-trip check. Native checks compare the complete stage 1 and 2 archives byte for byte, and the full campaign checks the arena code images after loading.

Packed stage extents are 30–84% smaller. Retry retains the current scene (except the arena, whose code banks are replaced) and reads only font and voice data. Continue, Game Over and title reuse the UI archive. Static UI data occupies 311,296 bytes; the remaining selection-animation tail is fetched only when entering selection. This cuts the terminal-screen decode from approximately 1.78 MB to 311 KB.

The focused native fixture measures retry at 454 video frames, including 120 settle frames, and Game Over at 690 frames, including 240 settle frames. Game Over to title performs zero additional disc reads. These totals include fades, CD seeks and screen preparation, not just data transfer.

## Verification and remaining limits

The native SGX campaign passes all seven stages, the ending, Continue/Game Over, hero selection and power/drop-through checks. Frontend, sky/foreground rendering, motion, boss/projectile regression, cache-pressure and depth checks pass. The focused reported-issues check verifies text, 336 restored race BAT cells, projection tables, pause colors, retry caching and distinct title menu patterns. Reports and screenshots are in `build/sgx`.

Strict 60 fps for dense combat is **not yet achieved**. The six-enemy fixture presents 160 of 180 frames (53.33 fps average); its warped-world setup includes a cold cache. Settling the world before spawning the same enemies produces 163 of 180 (54.33 fps). Both have zero essential sprite overflow and reach 60 fps after the initial burst, but occasional upload spikes remain. Collision scans, protected-page skipping, LRU search pruning and longer interrupt-safe transfer bursts reduce work without removing actors or changing their AI. The strict `--require-60` check still fails and remains available.

The standard PCE build and native boot/pause/HUD smoke check pass. Its existing dialogue-restoration test completes its restoration checks but fails its final zero-overflow assertion with six refused sprite admissions at the warped camera fixture. That failure is not represented as a passing standard-PCE suite.
