# Saturn hero rendering fixes — 2026-10-01

The changes are confined to Saturn imports and `PLAT_SATURN` character code.

- Bake Saber, April and Colt PNG sheets as individual 64×64 drawing units. Previously each sheet was one unit, so drawing a cell fell back to clipping parts that spanned neighboring cells. The stored width padding and source geometry could expose other poses.
- Mask the upper half of Saber's split running legs during Saturn import, using the existing 38 + torso-bob hip boundary. The current source sheet has an unrelated fragment above the hip in cell 80. Source PNGs are not modified.
- Set Saturn pose offsets immediately. Use the same rounded screen anchor as the character, and round both shadow endpoints in screen coordinates. Fractional camera motion no longer changes the shadow width or its offset from the character.
- Center crouching shadows on the planted feet, including crouch shooting: Saber left +2 / right 0 pixels; April left +2 / right −1 pixels. These are the centers of the bottom 16 rows of cells 112–113 and 120–121, relative to origin column 32.

## Checks

`python3 tools/saturn/tests/hero_cells.py` passed: all 216 Saber, 232 April and 152 Colt cells reproduce their imported RGB555 pixels exactly, with transparent VDP1 width padding and no source mutation.

The host regression exercises the actual Saturn `hero_apply`, `character_animate` and `character_draw_shadow` code:

```sh
cc -std=gnu11 -O2 -ffunction-sections -fdata-sections -DPLAT_SATURN -DFX_NO_FLOAT -Isrc \
  tools/saturn/tests/hero_shadows.c src/character.c src/heroes.c src/fx.c \
  -Wl,--gc-sections -o /tmp/hero_shadows
/tmp/hero_shadows
```

It passed for both heroes and directions, running, crouching and crouch shooting, across 120 animation updates per case with independent fractional body and camera motion. The same test against the previous source fails on the pose-offset assertion.

Ymir's software renderer captured 2,400 Saturn fields for the previous Saber build, fixed Saber build and fixed April build. Logs cover all six running leg cells in each direction and crouch animations 40–43. Visual comparison shows the stray pieces in Saber's previous left run removed. Both heroes were checked crouching and shooting in each direction. Evidence is in `artifacts/saturn-hero-fix/`, including `left-run-comparison.png` and each capture's `ymir/game.log`. These are emulator checks; console hardware was not used.

The full `make -j8 -f Makefile.saturn disc` rebuild passed, including the no-soft-float link check. Output: `build/saturn/saber_rider.cue` and `.bin`. Build log: `artifacts/saturn-hero-fix/final-build.log`.

Existing unrelated worktree changes, including the edited source PNGs and their authoring tools, are excluded from this commit.
