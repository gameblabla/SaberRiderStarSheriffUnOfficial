# SGX stage 2 race projection

`current_sgx_issues.txt` reports missing rival cars and a corrupted race track.

The rebuilt baseline reproduced missing rivals. The original isolated race
renderer test failed at distance 540 with no rival SAT entries. Native
projection records contained invalid ground rows and depths despite intact
quarter-square and reciprocal tables.

SGX LTO promotes `add()`'s `sx`, `row` and `f` output locals into zero page.
The compiled caller passed short addresses `$006b`, `$006d` and `$006f` to
`project_point()`, which wrote through ordinary indirect pointers. HuC6280
indirect addressing uses those full addresses in hardware I/O, whereas the
caller's direct zero-page loads use `$206b`, `$206d` and `$206f`. The short
writes also change mirrored SGX VPC window registers. The baseline window 2
was 769 rather than zero.

`race_proj.c` now normalizes these three output pointers into console RAM in
SGX builds. Standard PCE uses no LTO zero-page promotion and retains its
existing projection code and bank budget.

`test_sgx_race.py` now checks the native starting field before clearing it for
the transition fixture. It checks projection rows/depths, submitted rival
patterns, VPC windows, steering and 12 seconds of road autopilot. The isolated
depth sweep checks all six baked sizes and expects culling once the entire car
falls below the 224-line display, rather than demanding offscreen SAT entries.

Verification:

- SGX disc build passed; standard PCE disc build passed.
- Full `make -f Makefile.pce test-sgx` passed: campaign, frontend, motion,
  rendering, regression, reported issues, race, cache pressure, depth,
  combat profile and herd checks. The combat profile records 52.33 fps in
  its dense fixture; this change does not resolve the existing dense-combat
  performance limitation.
- Focused native SGX race check passed: four projected starting rivals,
  immutable VPC windows, 12 seconds of driving, all six perspective sizes,
  byte-exact rival patterns, and opening/finish/boss/victory sky restoration.
- Emulator captures reviewed: `build/sgx/race-grid-0.png` and
  `race-driving-{1,6,12}.png`. Baseline: `race-natural-0-0.png`.
- Machine-readable results: `build/sgx/sgx-race-verification.json`.

The stage entry uses the retail transition after seeding the requested stage;
finish and boss transitions use seeded state, and the size sweep calls native
compiled rendering with fixed positions. This is emulator verification, not
a manual campaign playthrough or a physical hardware test.
