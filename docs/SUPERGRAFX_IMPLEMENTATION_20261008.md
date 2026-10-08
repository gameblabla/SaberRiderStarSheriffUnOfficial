# SuperGrafx implementation status

This records the SGX implementation and gameplay follow-up against `SUPERGRAFX_ARCADE_CDROM2_PLAN.md`.

## Builds

The regular PC Engine build remains the default:

```sh
make -f Makefile.pce OUT=build/pce all
```

The enhanced build uses a separate output directory so generated assets and flags cannot be mixed:

```sh
make -f Makefile.pce SGX=1 OUT=build/sgx all
```

The SGX build is intended to fall back on ordinary PCE hardware. It probes SGX after a VBlank; if detection fails, the VDC1/VPC setup is skipped and the UI loader uses the original one-VDC records. Both editions still require Super CD-ROM² and Arcade Card. The emulator wrapper accepts `Emulator(..., sgx=True)` to set `pce.forcesgx 1`. SGX boot and stage 6 gameplay have now been exercised in the accurate-core emulator; ordinary-PCE fallback and physical hardware remain unverified.

## Implemented

- Added explicit SGX asset/build selection and isolated default output paths.
- Added post-VBlank SGX detection, matched 320×224 menu timing for VDC1, VPC mode 1 (`0x77` in each priority byte), neutral windows, a packed VDC1 index shadow, and hidden VDC1 control until a paired screen is ready. Failed detection does not issue VDC1/VPC writes after the probe.
- Added `arcade_vram_to()` for VDC0/VDC1 and made the existing `arcade_vram()` remain a VDC0 wrapper. VDC1 transfers use 16-byte bursts and restore the TII destination. The separate `SGX1` diagnostic block counts successful VDC1 VRAM transfers and bytes; its 8-bit call count and 16-bit byte count wrap. The existing `SRPC` telemetry layout is unchanged.
- Gameplay VDC1 register writes now mask interrupts only across index selection and the two data bytes, preserving the caller's interrupt state. VBlank/HBlank also select VDC1 registers, so this prevents a raster callback from splitting a CPU register transaction.
- Added one shared-palette paired-BG encoder. It makes complementary 4bpp planes with index zero transparent and emits reconstructed previews plus `sgx_static_report.json`. Stages 1 and 3 use it for a transparent main playfield on VDC0 and the source SkyBG on VDC1; stage 3's sky keeps its night tint. Both layers share the emitted palette, and the VDC1 sky map is uploaded once as a static 64×32 BAT.
- Paired the title background and all 19 victory paintings. The title keeps its live menu sprites and patches. A successful load uploads both tile sets and both BATs while VDC1 is hidden, then enables its BG after the regular fade-in. Any pair-load failure falls back to the original one-plane record.
- Added a two-VDC gameplay background path for stages 1, 3, 4, 5, 6, and 7. Stages 1 and 3 keep the scrolling playfield on VDC0 and use the source SkyBG on VDC1 with a slow SGX-only 3% camera drift; stage 3 retains its night tint. Stages 4 and 5 keep the scrolling world on VDC0 and use the source SkyBG on VDC1 at its slower parallax rate; stage 4 uses a 64-column BAT and stage 5 a 128-column BAT. Stage 6 now streams its panorama and raster floor on VDC1. Stage 7 scrolls its space map on VDC1 while VDC0 supplies the hull overlay. Normal display-off and fade calls hide/show VDC1 with VDC0. Stage 2's road raster remains on the established VDC0 renderer.
- Added a stage 6 SGX sprite-budget prototype. Near objects, enemy plasma, bolts, the player and HUD stay on VDC0; other objects beyond the 420-unit split use VDC1's separate SAT and scanline admission. If an optional far object cannot fit VDC1, it retries on VDC0 using that engine's saved occupancy. Stage 6's dedicated arm, large-mech and bolt patterns are copied to both VRAMs; ordinary cached patterns are copied to VDC1 on first use. The alternate VDC1 SAT is submitted alongside VDC0's SAT generation, and pause hides VDC1. The shared fast emitter now addresses either CPU SAT page.
- Added the first stage 6 tiled software-object path. The nearest large mech's 96 poses/scales are baked as BG-format 8×8 tiles and tile offsets. Two 256-character patterns pages and two BAT halves on VDC0 alternate; the page selection is committed with the SAT at VBlank. Palette 15 is reserved for the mech and the asset bake rejects stage 6 maps that use it. Other arena actors, the arm, HUD and effects still use hardware sprites, with the previous sprite renderer retained as a fallback if a BG upload fails. The SGX renderer body now lives in CD-RAM bank 135 at `$c000`; M6 saves MPR6, maps bank 135 for the call, and restores the previous bank. The same wrapper now clears the arena BAT after the stage 6 story, invalidates both cached object pages and restores the published page state, removing the dialogue glyphs before gameplay resumes. This recovers 2,556 bytes in overlay bank 113.
- Stage 7 uses its SGX-specific background map and 10 shared BG palettes, keeps the space scroll running while the boss is active, and draws the hull on VDC0 over the space plane without the legacy black fade. Hull-hit whitening is restricted to its reserved palette range.
- Reassigned the archive loader to overlay bank 113 and the UI mode body to bank 114 to make room for SGX IRQ and VDC1 gameplay code without editing the SDK linker script. VDC1 static-sky upload and column streaming use overlay bank 110. Loader arguments use the existing work-RAM bank rather than growing the guarded console BSS.

The generated SGX report from this build measured 42 visible colors and mean squared RGB error 4.12 for the title. The victory paintings contained 51–105 visible colors; their mean squared error ranged from 7.44 to 190.76. The largest encoded plane used 1,108 tiles on BG0 and 674 on BG1, within the 1,536-tile UI region per VDC. `ui.bin` was 210,944 bytes and `victory.bin` was 1,777,664 bytes, below the 1,966,080-byte per-file Arcade Card limit. The report contains per-screen values.

The stage 4 and 5 sky records are generated in their stage archives and loaded to VDC1. Stage 5's 128-column BAT occupies words `$0000–$0fff`, so its patterns start at `$1000`; the archive retains a compact sky descriptor in the existing stage metadata field. Stage 4 and 5 sky scroll values are derived from the world camera and published with the frame's SAT generation. The new SGX stage 6 archive is 1,476,608 bytes, below the 1,966,080-byte per-file Arcade Card limit. Its BG mech poses use 75–177 tiles each, within their 256-character pages.

## Not implemented yet

- The select, options, continue/credits panel, and game-over screens still use the original single-BG path. Their changing palette assignments need to be reconciled with the shared SGX background palette before pairing them.
- Stages 1 and 3 split out only the source's static SkyBG, with a deliberate 3% SGX camera drift. Stages 4 and 5 split out their SkyBG at the source parallax rates, but foreground-to-actor occlusion mirrors and VDC1 sprite/cache/admission remain absent. Stage 5's source export has no populated foreground tilemap.
- Stage 6 has a single nearest-mech BG prototype, not a general multi-object compositor. Its other actors remain sprites, and the present split does not yet preserve every depth relation when BG objects overlap foreground sprites. Dynamic actor ownership, detailed software-draw admission telemetry, complete arm/effect conversion and timing/raster profiling remain open.
- Stage 7 has one scrolling space BG plus the VDC0 hull overlay, not two independently composited space planes, a larger hull, or supplementary VDC1 hull pieces. The other illustrated menus still use the original single-BG path.
- The current SGX retail map uses 5,636 bytes of bank 113 (2,556 free) and 2,717 bytes of bank 135 (5,475 free). The tightest other overlay sections are bank 106 (8,067/8,192), 107 (8,163/8,192), 108 (8,166/8,192), 110 (8,115/8,192), 114 (8,176/8,192), 115 (8,183/8,192), 116 (8,067/8,192), 119 (8,187/8,192), 120 (8,103/8,192), and 124 (8,189/8,192). The previous software renderer placement in bank 113 left only 3 bytes there; code is now loaded as part of the app ELF in bank 135. This is CD-RAM, not the stage archive's four M6 code images.

Both the SGX and regular PCE builds completed through ISO generation and passed the project's linker, memory-bound, and unresolved-symbol checks. Outputs are `build/sgx/saber_rider.iso` and `build/pce/saber_rider.iso`; stage 1, 3, 4, and 5 SGX composites are in `build/sgx/preview/`.

For the stage 6 runtime check, a separate release-configured build used `-DPCE_START_STAGE=6` and `OUT=build/sgx-stage6`. The accurate-core emulator ran it with SGX, Arcade Card and the hardware sprite limit enabled. After the stage 6 story, `SRPC` reported `ready=1`, `stage=6`, `load_error=0`, `dropped=0`, `essential_overflow=0`, `forbidden_reads=0`, `max_units=7`, and `sat_count=13`. `SGX1` reported zero failures; the arena BG page bit alternated during ten one-frame samples after teardown, showing that the bank 135 draw body republished changing BAT pages. The before capture, `build/sgx-stage6/stage6_arena.png`, showed the leftover `Y TO RIDE!` glyphs; the after capture, `build/sgx-stage6/stage6_arena_after.png`, shows those cells cleared with the arena scene restored. A separate stage 5-start image was seeded to clear and advanced through the retail victory UI into stage 6; the loader reached `ready=1` with the stage 6 story active. This exercises one real campaign transition, not the full campaign.

The regular SGX image was also booted with the accurate core in ordinary PCE mode (`pce.forcesgx 0`). Detection telemetry remained `SGX1` with `flags=0`, and stage 1 reached `ready=1`; `load_error`, `dropped`, `essential_overflow`, and `forbidden_reads` were all zero. This verifies the combined image's basic PCE fallback on the emulator, not the full PCE campaign or physical hardware.

The full campaign, VPC ordering fixtures, stage 6 object depth/admission stress, VDC1 transfer timing, and physical hardware remain unverified. The stage 6 software-object path is a prototype, and the foreground, actor-allocation, and final-stage gaps above mean the SGX gameplay plan is not complete.
