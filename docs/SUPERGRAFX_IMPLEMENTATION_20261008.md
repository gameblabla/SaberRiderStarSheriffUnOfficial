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

The SGX build is intended to fall back on ordinary PCE hardware. It probes SGX after a VBlank; if detection fails, the VDC1/VPC setup is skipped and the UI loader uses the original one-VDC records. Both editions still require Super CD-ROM² and Arcade Card. The emulator wrapper accepts `Emulator(..., sgx=True)` to set `pce.forcesgx 1`; hardware and emulator boot behavior have not been exercised in this change.

## Implemented

- Added explicit SGX asset/build selection and isolated default output paths.
- Added post-VBlank SGX detection, matched 320×224 menu timing for VDC1, VPC mode 1 (`0x77` in each priority byte), neutral windows, a packed VDC1 index shadow, and hidden VDC1 control until a paired screen is ready. Failed detection does not issue VDC1/VPC writes after the probe.
- Added `arcade_vram_to()` for VDC0/VDC1 and made the existing `arcade_vram()` remain a VDC0 wrapper. VDC1 transfers use 16-byte bursts and restore the TII destination. The separate `SGX1` diagnostic block counts successful VDC1 VRAM transfers and bytes; its 8-bit call count and 16-bit byte count wrap. The existing `SRPC` telemetry layout is unchanged.
- Added one shared-palette paired-BG encoder. It makes complementary 4bpp planes with index zero transparent and emits reconstructed previews plus `sgx_static_report.json`. Stages 1 and 3 use it for a transparent main playfield on VDC0 and the source SkyBG on VDC1; stage 3's sky keeps its night tint. Both layers share the emitted palette, and the VDC1 sky map is uploaded once as a static 64×32 BAT.
- Paired the title background and all 19 victory paintings. The title keeps its live menu sprites and patches. A successful load uploads both tile sets and both BATs while VDC1 is hidden, then enables its BG after the regular fade-in. Any pair-load failure falls back to the original one-plane record.
- Added a two-VDC gameplay background path for stages 1, 3, 4, 5, and 7. Stages 1 and 3 keep the scrolling playfield on VDC0 and pin the static sky on VDC1. Stages 4 and 5 keep the scrolling world on VDC0 and use the source SkyBG on VDC1 at its slower parallax rate; stage 4 uses a 64-column BAT and stage 5 a 128-column BAT. Stage 7 scrolls its space map on VDC1 while VDC0 supplies the hull overlay. Normal display-off and fade calls hide/show VDC1 with VDC0. Stage 2's road raster and stage 6's arena remain on the established VDC0 renderer.
- Stage 7 uses its SGX-specific background map and 10 shared BG palettes, keeps the space scroll running while the boss is active, and draws the hull on VDC0 over the space plane without the legacy black fade. Hull-hit whitening is restricted to its reserved palette range.
- Reassigned the archive loader to overlay bank 113 and the UI mode body to bank 114 to make room for SGX IRQ and VDC1 gameplay code without editing the SDK linker script. VDC1 static-sky upload and column streaming use overlay bank 110. Loader arguments use the existing work-RAM bank rather than growing the guarded console BSS.

The generated SGX report from this build measured 42 visible colors and mean squared RGB error 4.12 for the title. The victory paintings contained 51–105 visible colors; their mean squared error ranged from 7.44 to 190.76. The largest encoded plane used 1,108 tiles on BG0 and 674 on BG1, within the 1,536-tile UI region per VDC. `ui.bin` was 210,944 bytes and `victory.bin` was 1,777,664 bytes, below the 1,966,080-byte per-file Arcade Card limit. The report contains per-screen values.

The stage 4 and 5 sky records are generated in their stage archives and loaded to VDC1. Stage 5's 128-column BAT occupies words `$0000–$0fff`, so its patterns start at `$1000`; the archive retains a compact sky descriptor in the existing stage metadata field. Stage 4 and 5 sky scroll values are derived from the world camera and published with the frame's SAT generation.

## Not implemented yet

- The select, options, continue/credits panel, and game-over screens still use the original single-BG path. Their changing palette assignments need to be reconciled with the shared SGX background palette before pairing them.
- Stages 1 and 3 split out only the source's static SkyBG. Stages 4 and 5 now split out their SkyBG at the source parallax rates, but foreground-to-actor occlusion mirrors and VDC1 sprite/cache/admission remain absent. Stage 5's source export has no populated foreground tilemap.
- Stage 6 does not yet have the software-object BG renderer. It deliberately stays on the original VDC0 arena path so the SGX gameplay switch cannot blank the floor or route its raster to an uninitialized plane.
- Stage 7 has one scrolling space BG plus the VDC0 hull overlay, not two independently composited space planes, a larger hull, or supplementary VDC1 hull pieces. The other illustrated menus still use the original single-BG path.
- The latest SGX link leaves 18 bytes in bank 104, 25 in bank 106, 44 in bank 107, 26 in bank 108, 51 in bank 110, 96 in bank 113, 9 in bank 115, 5 in bank 119, and 3 in bank 124. The guarded `.bss` plus software-stack reservation ends at its `$3bf0` boundary. Further code needs another allocation or code/data reduction.

Both the SGX and regular PCE builds completed through ISO generation and passed the project's linker, memory-bound, and unresolved-symbol checks. Outputs are `build/sgx/saber_rider.iso` and `build/pce/saber_rider.iso`; stage 1, 3, 4, and 5 SGX composites are in `build/sgx/preview/`. No emulator capture or physical-hardware check was run. VPC ordering, PCE fallback at runtime, gameplay transitions, and VDC1 transfer timing still need runtime verification; the enhancements listed above as unimplemented are not complete acceptance of the full plan.
