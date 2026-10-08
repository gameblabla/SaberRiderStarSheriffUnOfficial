# SGX presentation and combat follow-up — 2026-10-08

This completes the SGX sky/front-end work left in the working tree by the prior session, plus the six follow-up requests. Unrelated captures, logs, assets and other-platform work remain outside the commit.

## Changes

- Title: ordered dithering before fitting the two VDC palettes. The source painting and logo remain the original art. The baked screen uses 62 VCE colors; the standard PCE final composition uses 49. Pixel MSE is 212.50 against 468.19 for that PCE composition. Dithering deliberately trades pixel error for smoother perceived gradients; it is not claimed to lower MSE relative to the previous undithered SGX painting.
- Selection: 36 baked 10-degree phases rotate the spiral and moon in opposite directions, matching the source motion. Pixels hidden by all four opaque portrait maps are omitted from the backdrop. Each phase needs at most 578 patterns; two independent pattern pages start at word $1000/$4000, and two BAT pages at $0000/$0800. Transfers stream from already loaded Arcade RAM in 2048-byte chunks. A completed page becomes visible during VBlank. All ten foreground palettes remain immutable. There is no menu-time CD read.
- Level-1 stampede: VDC0 foreground and its ground tiles stay at Y=0; the herd/actors on VDC1 retain their shake. Foreground pieces no longer lift away from the ground.
- Saloon props: the scenery pass never retries on VDC0, and rear props move behind fighters in VDC1 SAT order. A stale front-plane hint cannot put a saloon prop above the actors or VDC0 scenery.
- Combat: retained foreground preparation runs only at chunk-window boundaries; stationary grounded enemies reuse the immutable floor contact; VDC budgets use independent arrays instead of repeated 240-byte copies. Admission code switches the high bytes of its absolute operands (interrupt handlers never access these budgets). Repeated-span cache state retains both its row and remaining capacity per VDC. VDC0 SAT uploads clear formerly live entries per alternating source and skip already-hidden tails; pause/retirement mark externally replaced tables conservatively. The linker guards the operand page layout. These changes retain the normal per-VDC sprite limits.
- Game over: the full Nemesis painting and additive lettering are fitted across both VDC background planes, using 74 colors. The old four-palette lettering pulse is disabled for this paired painting because those palettes now also own painting pixels. The standard PCE glow/pulse path remains intact.

Animation code executes in bank 135 through MPR6 ($C000), while its state remains in the always-mapped work bank. New ELF guards check that mapping and the bank-$6E admission switch. Both display mode and pattern/BAT storage are reset by the normal menu/gameplay loaders.

## Performance evidence

`tools/pce/test_sgx_combat_profile.py` boots the retail disc, seeds six living enemies (with enough HP to survive the fixture), holds fire, and alternates walking direction over 180 video frames. Native simulation, animation, collision and sprite admission continue to run. Between three and six actors remain active. The accurate Mednafen core has Arcade Card/SGX enabled and hardware sprite limits enabled.

| Build | New presented SAT generations / video frames | Rate |
|---|---|---|
| Starting working tree | 74 / 180 | 24.67 fps |
| Final independent-budget/SAT-tail implementation | 111 / 180 | 37.00 fps |

The fixture improves 50%. It still falls below 60 Hz in this fight; no general 60 Hz claim is made. The fixture is debugger seeded, not a manual playthrough. Initial and final profiles/captures are in `build/sgx/combat-before*` and `combat-sat-tail*` (the full suite emits another `combat-after*` profile).

The three focused stampede checks passed, with foreground shake values `[0]` in all three, 64/20/45 retained foreground parts checked, and measured presentation rates 58.71/59.85/59.89 fps on the final full-suite image. The overlap fixture passed for all five source scenery types (13, 14, 15, 17, 18), including both nine-piece saloon sprites. Every prop remains on VDC1 and behind the six-piece overlapping fighter.

## Verification

`make -f Makefile.pce test-sgx` passed in one complete final run, including campaign scenarios through the ending, front-end/rapid switching/game over, motion/repeat/BAT boundaries, rendering/projectiles/death, boss/cache regressions, stage-7 cache pressure, all five scenery depth fixtures, combat cadence, and all three stampedes. Native idle and stationary firing each measured 60.0 Hz. The first broad run stopped on incorrect stage-6 telemetry: the new stats reader had selected the platform-only scratch budget for the arena. It now reads that scratch budget only while the platform split pass is active; the complete final run passes without relaxing the limits.

Both `SGX=1` and `SGX=0 RETAIL=1 OUT=build/pce` retail builds passed ELF/bank guards and disc generation. `git diff --check`, staged diff checks, and Python compilation checks passed. The standard PCE full regression suite was not rerun; its earlier failures remain documented in the prior session. Native front-end captures are in `build/sgx/sgx-frontend-review/` (title, selection phases/switches, game over). Native saloon capture: `build/sgx/saloon-depth-14.png`. Source/baked previews: `build/sgx/preview/`. Build/test logs: `build/sgx/presentation-combat-verification/`.

Not tested on physical hardware. The broader standard-PCE regression failures recorded in the prior session were not reclassified or silently removed by this SGX task.
