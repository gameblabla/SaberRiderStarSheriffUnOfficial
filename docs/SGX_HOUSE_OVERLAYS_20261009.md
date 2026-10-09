# Remaining house overlays

The supplied `saber_rider.29fa56a6477076d9475483f9199352c2.mc0`
reproduces two fan cells across the hotel's left window/edge. Captured with
`loadstate` followed by one native frame (`/tmp/house-reported.png`).

The previous occlusion mask clipped the flattened background copy, but left
an independently drawable, full fan sprite. Masking the bake alone therefore
could not remove the overlay.

`tools/pce/build_assets.py` now excludes scenery types 17 and 18 from both
`scenery_background()` and the actor sprite export. Their actor IDs are 255
(the existing unavailable-asset sentinel) in every platform scene, for both
PCE and SGX. The generator preserves the original tilemap artwork beneath
them without painting replacement patches. Other scenery retains its existing
behavior.

The house regression now compares both entire 48x32 fan regions against the
unmodified tilemap composition and rejects drawable fan assets in the manifest.
The scenery-depth check covers the three retained scenery types instead of
requiring the removed overlays to appear.

Both `make -f Makefile.pce SGX=1 all` and `make -f Makefile.pce all` passed.
Fresh native captures `build/sgx/hotel-disc-fixed.png` and
`build/sgx/saloon-disc-fixed.png` were visually reviewed: both facades are clear
of fan overlays. The existing emulator state retains its old code/assets;
use a fresh boot of the rebuilt disc to see the generator changes.

Focused checks passed: `test_sgx_house_moon.py`, `test_sgx_depth.py`, and
`test_sgx_rendering.py` against `build/sgx`. Python compilation and
`git diff --check` passed. No physical hardware validation or full suite rerun
is claimed for this follow-up.
