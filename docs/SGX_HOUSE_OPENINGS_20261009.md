# Recurring hotel/saloon window corruption

The supplied `saber_rider.a57f813c277bc355fcaec5298e34731a.mc0` restores
the reported hotel patch in the accurate Mednafen core. A fresh boot of the
pre-fix working-tree disc also shows it. The visible VDC0 BAT and character
data match the stage archive: this occurrence is baked artwork, rather than
an incorrect tile-cache upload.

`Playfield` deliberately leaves 48x32 openings for source window props:
type 17 at `(6544,80)` on the saloon and type 18 at `(7168,80)` on the hotel.
Removing both their actor and flattened background copies leaves the openings
exposing `MidBG`, including parts of a different building moved by the
continuous parallax bake. Checking that the output equals the unfilled tilemap
there therefore validated the defect. Merely clearing these cells would also
leave an incorrect transparent opening.

The generator now keeps the static source window frames in the background.
It clips every prop against the opaque tile layers between `MidBGHyperjpr`
and `PlayerSprites`, preserving the facade in front. Types 17/18 remain
unavailable as runtime actor sprites, so a full independently drawn window
cannot overwrite the facade, change with sprite admission, or expose the
opening when refused. This treatment applies to both SGX and ordinary PCE
stage-1 background bakes.

`tools/pce/test_sgx_house.py` independently reconstructs the terrain mask
and source window frames. It checks all protected facade pixels and all 1,536
opaque pixels in each opening, and rejects the old unfilled output. It also
checks that the quantized window characters contain no transparent pixels,
that no window actor assets are available, and that native VDC0 patterns and
palette assignments match the generated archive. Coverage includes both
buildings, forward/reverse seeded camera positions across tile/sector
boundaries, and 90 consecutive native presentations while moving and firing
through the hotel. The test is included in `test-sgx`; the SAT-depth test now
covers retained actor props, with static windows covered by the new test.

The existing untracked house/moon test was adjusted to reject missing window
fills instead of demanding equality with the unfilled source. Unrelated
pending renderer, loader, moon, race, and frontend changes are preserved and
are not included in this commit.

## Verification

- The new source check fails against the pre-fix working-tree generator on
  the saloon opening and passes with the repair.
- SGX and standard-PCE disc builds pass.
- The focused native SGX test passes 114 samples / 91,884 background cells.
  Its moving samples cover 90 different camera positions from 7063 to 7363.
- Fresh `build/sgx/hotel-disc-filled.png` and
  `build/sgx/saloon-disc-filled.png` were visually reviewed. Both windows
  contain their matching source art, with no unrelated building patch.
- A fresh native standard-PCE boot, with the ordinary campaign dialogue
  dismissed, verifies 1,716 BAT/pattern/palette cells across both buildings.
- Python compilation and whitespace checks pass.

The broad `make -f Makefile.pce test-sgx` run completed twelve checks before
the last, herd check, failed a stale sprite selector: it treated every
32-pixel-wide VDC1 sprite as a horse. The failing entries included valid hero
patterns outside the protected horse pages. The fixture now identifies horses
with their reserved palette, while retaining the bidirectional page-ownership
check, exact pattern bytes, cell sizes, coordinates, animation poses, and
rendered-horse requirement. The separate herd rerun passes all three convoys,
including continuous firing, resident pattern integrity, foreground retention,
and camera catch-up. All thirteen scripts pass across the broad run and this
focused rerun; the whole target is not claimed to have exited successfully.

The generic standard-PCE `test_port.py` was also attempted against the retail
disc and failed its initial walking-distance assertion. That fixture uses
the diagnostic menu; the retail working tree has DEBUG disabled. This is not
a passing standard-PCE full-suite result, nor proof of a pre-existing failure.

Old Mednafen states preserve old code, Arcade RAM assets, and VRAM. Boot the
rebuilt `build/sgx/saber_rider.cue` to see the repair; loading the supplied old
state alone restores its old artwork. Validation is emulator-based; physical
hardware was not tested.
