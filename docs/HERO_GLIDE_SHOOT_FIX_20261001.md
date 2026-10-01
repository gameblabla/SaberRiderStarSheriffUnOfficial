# Falling sprites and diagonal shot origins

The shared character code now joins Saber's and April's falling torsos to their
pelvis, including walking off a roof, dropping through it, and airborne aiming.
Selecting cells 72/96 alone did not correct the reconstructed artwork's offset.

| Hero | Level/diagonal fall torso L / R (x,y) | Additional vertical-aim y L / R |
| --- | --- | --- |
| Saber | (3,1) / (-3,1) | 4 / 4 |
| April | (0,3) / (0,2) | 7 / 6 |

Diagonal muzzle positions come from the barrel-tip pixels of the current PNG
cell, including recoil, torso offsets and Saber's run bob. Standing cells and
running/falling torso cells have separate coordinates. Shots and muzzle flashes
spawn after the current pose has been resolved, moved and animated. This also
corrects the first shot when turning or changing aim on the shooting step.
Dark April uses the same sprite alignment and updated firing order. A character
killed or locked during that step cannot emit its pending shot.

The changes apply to the shared core used by Linux, Windows, WebAssembly,
Dreamcast and Saturn. No PNG modifications are required.

## Validation

```sh
make -f Makefile.headless check-heroes
make -f Makefile.headless FIXED=1 check-heroes
python3 tools/check_hero_poses.py build/headless/hero_poses_test --sheet /tmp/hero-poses.png
```

Each arithmetic mode checks 4,032 poses against the actual CRHC definitions and
PNG pixels: both directions, all airborne aim directions, shooting/recoil, and
six running poses. The checks require a connected five-pixel belt/pelvis seam,
verify diagonal muzzle positions against independently measured barrel pixels,
exercise the roof-drop state transition and check all four diagonals for all
four heroes when turning/aiming/shooting on the same real game update. The
generated sheet shows cyan shot rays from the barrels.

Both modes passed, including the committed PNGs as well as the existing working
PNGs. Rebuilding the previous HEAD failed the same-step shot-origin assertion;
checking its falling poses separately failed Saber's pinched hip seam. The
rendered pose sheet and fresh Linux gameplay screenshots of both heroes falling
and shooting diagonally were reviewed visually. Linux, Windows, WebAssembly,
Dreamcast ELF and Saturn BIN/CUE builds completed. Console hardware/emulator
playthroughs were not performed for this change.
