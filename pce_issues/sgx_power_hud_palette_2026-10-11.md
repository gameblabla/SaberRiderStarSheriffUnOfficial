# SGX HUD palette after a power skill

The reported `saber_rider.8ffed3d7337b30fcb72f949f34ee5894-pal.mc0`
contains the three-colour counter palette in VCE sprite palette 31. Its
runtime layout differs from the current ELF, so verification uses a rebuilt
retail disc with a seeded stage-5 scenario and ordinary Select-tap input.

Before the fix, tapping Select spends a power (2 -> 1), loads the uncached
`hud_digit1`, and replaces palette 31 with
`0000 ff01 b601 0000 ...`. Cached HUD portraits/health bars still refer to
that shared palette. The cut-in snapshots it and faithfully restores the
incorrect colours; waiting for a later HUD cache miss only masks the bug.
A native input probe reproduced this on the starting revision, whereas
forcing CAM_POWER directly without spending a power did not reproduce it.

The baker previously unified only `hudp_` palettes (race/cockpit/space).
Platform `hud0_` through `hud3_` and `hud_digit` records now use the same
stage-wide palette too. Both their patterns' indices and their palette
records are baked together, so loading any counter cannot recolour another
cached HUD piece. Fade timing and runtime ownership are unchanged.

Regression: `tools/pce/test_sgx_power_hud.py` checks the shared palette
records for all four platform stages, then triggers both stage-5 powers
through Select taps and compares all 16 HUD colours for 90 emulator frames
after each cut-in. Captures and the JSON report go into `build/sgx/`.

Validation passed:

- `make -f Makefile.pce SGX=1 all`
- `python3 tools/pce/test_sgx_power_hud.py --out build/sgx`
- `python3 tools/pce/test_sgx_last2.py --out build/sgx --only-stage4`
- `git diff --check`

These are focused accurate-core emulator checks; the full suite and physical
hardware were not tested.
