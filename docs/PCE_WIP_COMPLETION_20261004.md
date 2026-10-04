# PCE WIP completion — 2026-10-04

This completes the remaining work from `2026-10-04-222623-local-command-caveatthe-command-below-was-run-d.txt`, plus the split-mountain save and missing wall tile screenshot. The existing level 1/3 sky treatment is retained.

## Final behavior

- Mountains use intact source strips at native scale instead of sector joins through solid scenery. Short strips extend with a reflected copy, so their boundary has no discontinuity. Buildings retain the existing placement and gap-based joins so scenery actors stay aligned.
- The security-camera wall repair accepts transparent sky as well as blue sky. Its removed sprite-wall pixels now have permanent scenery behind them, including during stampede rendering. Both original camera animation frames remain available.
- Platform dialogue corners have four dedicated sprite patterns at VRAM words `$4100–$41ff`, above the nine panel characters and below the font. They use sprite palette 31 while dialogue is open and never request cache slots. The shared foreground/horse palette is prepared before the closing SAT and restored at VBlank. This leaves boss palette 30 intact and prevents the last corner half being refused by the actor cache.
- The level 7 cruiser is 180×98 pixels, baked to 146 background characters with four palettes in the preloaded Arcade archive. A palette fade replaces the nebula with a black BAT; BXR/BYR move the hull. No cruiser sprite patterns are loaded. Hull-column outlines drive collision, port heights scale with the larger art, and touching the hull pushes the player clear. The fight plays source track 11. Pending waves stop on entry. Bombs and powers flash all palettes white, play a death grunt and explosion, then restore the scene. The existing destruction sequence and ending dialogue remain.
- The race has cloud wisps from the source panorama and a distant ridge over its blue gradient. BXR changes at scanline 80, with a slower cloud band and a faster horizon band; the road still starts at scanline 120. Two repeated 512-dot sky copies fill the 1024-dot BAT, so scrolling never reveals uninitialized columns. Dialogue holds both sky offsets at zero until gameplay resumes.
- Horse animation uploads now use four equal 1280-byte slices. The fourth completes the inactive buffer just before publication. This removes the firing-start slowdown found at the third stampede without writing displayed patterns.

The previous WIP behavior is retained: bottom dialogue for the outrider cutscene; scenery/font restoration when dialogue closes; offscreen entrances for stages 3–5; cleared drop-through state; running aim poses when falling off a ledge; projectiles stopped by solid cells; gliding boss camera locks; saloon props behind fighters; removed race control card; pursuit/boss tracks 14/17; separate enemy shot slots; road-centered escort/leader steering; reduced leader health and speed; pursuit damage before the catch; turbo flames, refill and half-full unlock; and the two-row pursuit HUD.

## Memory and test fixtures

Scenery drawing uses the new `$78` overlay. CD scratch is now `$76–$77` (16 KiB), preserving that overlay through every scene reload. Stage reads therefore use more BIOS transfer calls; input helpers allow the longer load. There are no disc reads during the cruiser fight. The new overlay keeps renderer and mission banks below their 8 KiB limits.

Native test trampolines moved from `$3b00`, which now overlaps application BSS, to reserved `$3bf0–$3bff`. The ELF check protects this space. Foreground tests check unchanged actor patterns in both facings on stage 5, whose source has no foreground objects; stages 1, 3 and 4 intentionally remove their foreground. Boss warps seed the camera with the player, while the native arena lock still glides. The campaign check expects the outrider panel at the bottom, hits the larger cruiser inside its hull, and verifies jumping back after a platform drop.

## Verification

All 17 checks listed in `make -f Makefile.pce test` pass on the rebuilt disc. The initial invocation passed through the third stampede profile, then stopped because the foreground fixture assumed stage 5 contained scenery sprites. After correcting that assumption, `make -f Makefile.pce test-presentation test-audio` passed the remaining six checks. The campaign was also rerun with the new jump-back assertion. Run logs are saved as `build/pce/wip-regression-{main,remaining,campaign}.log`.

Each stampede presents 300 new frames in 300 VBlanks while firing, with zero essential overflows. The herd renderer passes 160 SAT/occupancy cases and 32 HUD cases; visibility passes 1024 draws covering all four heroes and every horizontal phase. Dialogue restores 990 cells, the original font and palette at each of three scroll phases. Presentation covers sixteen aim poses and four restarts through the title/selection flow. Audio passes all four heroes' events, concurrent shot/gallop playback and shutdown; native codec comparisons cover 954 decoder samples and 1590 software playback samples. The rebuilt ELF passes bank/RAM limits, Python files compile, and `git diff --check` passes.

These are accurate-core, hardware-limited emulator scenarios with explicit debugger seeds, not an unassisted playthrough or a physical-disc timing certification.

Review captures: `build/pce/wip-review/` (first dialogue; level 1 at the mountain and camera locations; race panorama; background cruiser; white bomb flash; restored black arena; moving hull).

New native assertions: `build/pce/wip-verification.json`. The check verifies dedicated corner entries, repeated sky BAT halves, exact cruiser BAT/pattern bytes, absence of cruiser cache allocation, no fight-time disc reads, white palette flash/restoration, and hull scroll movement.

Additional reports include `verification.json`, `campaign-verification.json`, `dialog-restore-verification.json`, `foreground-verification.json`, `herd-render-verification.json`, `herd-visibility-verification.json`, and `gameplay-profile-herd{1,2,3}.json` under `build/pce/`.
