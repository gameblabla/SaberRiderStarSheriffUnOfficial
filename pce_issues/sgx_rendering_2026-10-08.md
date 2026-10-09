# SGX rendering repairs — 2026-10-08

Historical checkpoint. The current sky motion, foreground/hull palettes,
independent VDC sprite allocation and final verification are documented in
`sgx_camera_sprite_regressions_2026-10-08.md`; use that report for the current
implementation.

## Changes

- Corrected the normal/fast foreground camera selection in `fg_emit`.
  Foreground now occupies VDC0 once, ahead of world sprites. The hero and
  other platform actors use VDC1; this removes the overlap mirror pass.
- Expanded the SGX sprite lookup to 512 entries. Full foreground IDs 497
  and 499 previously exceeded the 480-entry lookup.
- Separated foreground (VCE 30), HUD (31), and herd (29) palettes; platform
  actors use palettes 16–28. Herd reservation preserves VDC0 scenery.
- Background column writes preserve VDC1 sprite enable. They previously
  disabled the plane carrying projectiles and enemy death sprites.
- Kept complete converted sky panoramas in Arcade RAM: stages 1/3 are
  976 pixels wide, stage 4 is 768, and stage 5 is 1824. A 33-column BAT ring
  streams referenced characters into an independent 896-character VDC1
  cache. Wrapped source skies remain stationary; other skies retain their
  source camera ratios. Stage 5 retains 2599 source characters rather than
  reducing the entire panorama to one static VRAM image.
- Removed unnecessary memory copies, repeated foreground admission, full
  VDC1 SAT tail transfers, and full-cache scans on sky column changes.
  Short HuC6280 block transfers retain PCM IRQ service opportunities.
- SGX platform scheduling consumes an available simulation tick after a
  draw crosses VBlank, avoiding a second wait that sustained 30 fps.
- Stationary herd-stage skies contain at most 512 characters and end at
  VRAM word $2800. Three horse poses reside at $2800/$3200/$3c00; the
  other two occupy the reserved $6400/$6e00 sprite buffers during a convoy.
  There are no horse pattern transfers during the convoy. All five poses
  still use the original simulation-frame animation timing.
- Moved the virtual arena A image from linker bank 128 to 144, freeing
  physical CD-RAM bank 128 for SGX helpers. Arena A still executes from
  its original runtime overlay bank. The IPL's physical load range remains
  unchanged.

## Measurements and checks

Accurate-core emulator, forced SGX, Arcade Card enabled, hardware sprite
limits enabled. Measurements count completed presentations rather than host
emulator speed.

| Stage 1 fixture | Initial SGX | Repaired SGX |
| --- | ---: | ---: |
| Idle, 600 video frames after warmup | 28.4 fps | 60 fps |
| Continuous firing, same interval | 20 fps | 60 fps |
| Three native convoys, no held fire | — | 59.94 / 59.95 / 60 fps |
| Locked convoy with continuous firing, 300 video frames | — | 57.6 fps |

The focused rendering check compares actual VRAM characters and palette
selectors against all visible sky cells at multiple camera positions in
stages 1, 3, 4, and 5, and checks retained foreground coordinates/priority.
It also observes 30 consecutive projectile display generations and all six
walker death poses, including their actual patterns and palettes.

The herd check covers all three level 1 convoys, compares all five resident
pose buffers throughout each convoy, verifies palette 29, observes all five
poses, and checks camera locks, audio lifetime, right-edge map continuity,
disc-read counts, and sprite limits.

Commands:

```sh
make -f Makefile.pce SGX=1
python3 tools/pce/test_sgx_rendering.py --out build/sgx
python3 tools/pce/test_herd.py --out build/sgx --sgx
python3 tools/pce/profile_gameplay.py --out build/sgx --sgx
python3 tools/pce/test_campaign.py --out build/sgx --sgx
```

Generated reports and screenshots are under `build/sgx`. The campaign
scenario covers stage transitions, bosses, powers, arena code integrity,
ending, restart, and game over. PCE campaign regression also passed after
the arena linker relocation. These are emulator results; physical hardware
has not been checked. CD loading-time optimization remains deferred.
