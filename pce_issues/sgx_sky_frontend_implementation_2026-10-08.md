# SuperGrafx sky and front-end implementation

Date: 2026-10-08. Resumes `../../codex-session-01a11d14-bcf6-73b2-8e8b-732a842bf286.md` and implements [the follow-up](sgx_sky_frontend_followup_2026-10-08.md).

## Implemented behavior

- Level 1 crops only the trailing resolved-alpha padding: 976 source pixels become a 512-pixel/64-column repeat. The left origin, height, internal holes, and opaque black remain intact. Tile alignment fills from the repeat origin. Other stages retain their independently audited widths; the report includes 64-row band coverage for stage 4. Camera and BAT coordinates remain continuous.
- The title uses the original mirrored backdrop, shadow, and logo in RGB before shared-palette, complementary-mask two-VDC encoding. PCE-derived sprite patches are omitted on SGX; menu sprites/highlights remain. Source and reconstruction detail crops are emitted in `build/sgx/preview/`.
- Selection puts source spiral/moon art on BG1 and transparent frames/portraits/heading on BG0. All four foreground states share 849 resident patterns; the backdrop has 1,086. BG palettes 0–5 belong to the backdrop, 6–15 to the foreground. Switching uploads a 2,240-byte inactive BAT with no pattern or palette uploads, then publishes its Y scroll at VBlank. Names and arrows remain sprites.
- Both engines use the 320×224 menu mode. Fades operate on the complete shared palette. Exiting clears the paired-menu state and subsequent scene setup restores gameplay geometry and maps.

The interrupted session overflowed bank $7c by 244 bytes. Map transfer and portrait-switch bodies now run in $6e, using always-mapped transfer arguments and copied portrait-map offsets. Map rows increment addresses instead of multiplying a 32-bit row offset. The linked retail SGX image leaves 260 bytes in $6e and 172 in $7c. `check_elf.py` guards the new overlay entry bank and the existing menu entry banks.

## Focused evidence

`python3 tools/pce/test_sgx_frontend.py --out build/sgx` checks the decoded source extent independently of the baked record, source-preserving crop fixtures, both title planes, options/return restoration, all four portraits, ten switches, immutable foreground palettes and backdrop patterns, four pulse phases, rapid switching, and confirmation during a switch. It emits `build/sgx/sgx_frontend_report.json` and native captures in `build/sgx/sgx-frontend-review/`.

The original title has 59 RGB colors, mapping to 37 distinct VCE colors; the pair retains all 37. RGB reconstruction MSE is 96.43 versus 468.19 for the PCE baseline including its sprite patches, using the original title as the common reference. Both planes contribute (71,567 and 113 visible pixels). This measures fidelity rather than rewarding a larger color count from a synthetic backdrop.

`python3 tools/pce/test_sgx_motion.py --out build/sgx` passed and emits `motion-verification.json`. Added boundary fixtures capture camera positions 2020, 2044, 2048, 2052, 2076, then reverse through 2052/2048/2044, plus the second repetition at 4092/4096/4100. Every sample checks native scroll and all 990 visible sky BAT/pattern cells. Existing fixed-camera, camera-follow, BAT-wrap, and stage-4 split-band checks remain.

Native title, selection, repeat-boundary frames, and the encoded sky preview were visually inspected. The source skyline remains visible across the cropped repeat, with no padded flat interval; no additional seam artwork was authored.

## Limits and broader regression results

The source-derived selection backdrop is a detailed static rotation phase with an isolated palette pulse. Actual spiral/moon rotation is deferred: full phase replacement can require 35,840 pattern bytes before map transfers, and no acceptable animated transfer budget has been demonstrated. This follows the plan's initial static-backdrop stage. No physical-console playtest is claimed.

Both retail SGX and debug PCE images build successfully. After the debug checks, `build/pce/` was rebuilt with `RETAIL=1`, so both final disc outputs are retail builds. The generic PCE full-suite target requires `RETAIL=0`: running its debug-menu fixture against retail first failed the walking assertion. The obsolete planar-demo check was replaced with a native BAT-glyph check for the actual debug menu; that check passes. `test_port.py` subsequently fails its BIOS checkpoint-after-reboot assertion. The full PCE suite therefore is not reported as passing. Remaining checks were run independently; the final results and logs are recorded below.

### Final SGX results

All checks in the SGX regression group passed individually on the final retail disc:

| Check | Result |
|---|---|
| Retail build and ELF memory/overlay guards | Pass |
| Campaign scenarios, menu returns, all stages and heroes | Pass |
| Source crop and front end, 160 rapid-switch frames, confirm during switch | Pass |
| Camera motion, repeat boundaries, BAT wrap, split sky | Pass |
| Native rendering, fixed-camera/scroll, idle/fire cadence | Pass |
| Sprite admission, horse residency, boss firing regressions | Pass |
| Cache pressure and protected Arcade Card pages | Pass |
| Three locked convoys with foreground, sky, horse and bullet pressure | Pass after fixture synchronization fix |

The first `make -f Makefile.pce test-sgx` run stopped in the last herd check because it read `herd_locked` before `play_tick` refreshed telemetry, observing the previous player position two pixels before the stop window. `test_herd.py` now waits four video ticks for that simulation step to finish, without changing its stop bounds. A focused rerun passed all three convoys. The complete target was not repeated after this fixture-only edit; its preceding checks had already passed on the same runtime image.

The herd pressure run measured 56.90, 59.35, and 58.06 presented frames/s; do not interpret its pass as a 60 Hz guarantee. The separate native idle/fire rendering measurements were 60.0 Hz.

### PCE checks on the debug image

| Check | Result |
|---|---|
| `test_port` | Fail: checkpoint does not restore stage 2/pursuit after reboot |
| `test_campaign` | Pass |
| `test_wip` | Fail |
| `test_boss_presentation` | Fail |
| `test_boss_passes` | Pass |
| `test_priority` | Pass |
| `test_herd` | Fail |
| `test_herd_render` | Fail |
| `test_herd_visibility` | Fail |
| `test_foreground` | Pass |
| `test_presentation` | Fail |
| `test_dialog_restore` | Fail |
| `test_graphics_stability` | Fail |
| `test_audio` | Pass |
| `test_adpcm2` | Pass |
| `test_software_adpcm` | Pass |
| `profile 1` | Fail |
| `profile 2` | Fail |
| `profile 3` | Fail |

Failures include missing expected WIP panel corners, camera-light animation, PCE sprite admission under herd/dialog/scroll pressure, herd synthetic SAT/steady-HUD fixtures, and an options fixture expecting a different flag value. These checks ran against the current wider working tree; a clean-baseline comparison was not performed, so they are not classified as newly introduced or pre-existing defects. No broader PCE correctness or 60 Hz claim is made.

Saved logs: `build/sgx/resume-verification/`. Source and native front-end evidence: `build/sgx/sgx_frontend_report.json`, `sgx-frontend-review/`, and `preview/`. Sky/camera evidence: `motion-verification.json` and `motion-repeat-*.png`. Other SGX reports include `rendering-verification.json`, `sgx-regression-verification.json`, and `sgx-cache-pressure-verification.json`. `git diff --check` and Python compilation checks pass. Existing unrelated working-tree edits were preserved.
