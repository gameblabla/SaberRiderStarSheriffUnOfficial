# SuperGrafx sky cropping and front-end enhancements

Date: 2026-10-08. Status: implemented in the asset generator and runtime. Verification and remaining limits are recorded in [the implementation report](sgx_sky_frontend_implementation_2026-10-08.md).

Follow-up to [the previous session](../../codex-session-01a11cc0-1276-7970-bdd0-c4adc2df98a7.md) and [its verification report](sgx_camera_sprite_regressions_2026-10-08.md). The requested work is:

1. Correct the sky's source repeat period by removing unwanted right-side padding before conversion.
2. Make the SuperGrafx title visibly more colorful using both VDCs.
3. Give SuperGrafx character select the fancy background and a separate portrait/panel plane, avoiding the PCE composition problem.

## 1. Sky: confirmed source-content defect

The level-1 sky contains **464 trailing transparent pixels in a 976-pixel-wide rendered source**. Its visible art occupies x=0 through x=511. The current SGX conversion repeats the entire 976 pixels, including that empty tail.

This was measured by loading the current `build/sgx/work/stage1.layers`, resolving its tile bank through `levl`, rendering the full SkyBG to RGBA, and finding columns with alpha >= 128. The same alpha threshold is used by the graphics pipeline. Inspection of [the generated sky preview](../build/sgx/preview/stage1_sgx_sky.png) also shows the detailed planet/cloud art ending around the middle, followed by a large flat area. The preview's flat color represents the backdrop through transparent pixels.

| Stage | Rendered source width | Columns containing opaque pixels | Trailing transparent width | Interpretation |
|---|---:|---|---:|---|
| 1 | 976 px | [0, 512) | 464 px | Confirmed unwanted tail in the SGX repeat period |
| 3, exported SkyBG | 976 px | [0, 512) | 464 px | Same source map, but superseded by the night-sky override |
| 3, active override | 720 px | [0, 720) | 0 px | `assets/stage3/native/stage3_night_sky.png`; inspect its seam separately |
| 4 | 768 px | [0, 768) | 0 px | No completely transparent trailing columns in the full SkyBG |
| 5 | 1824 px | [0, 1824) | 0 px | No completely transparent trailing columns in the full SkyBG |

These are measurements of the current extracted assets, not fresh emulator results. Full-height coverage does not prove every visible row band is free from padding; inspect stage 4's upper sky and lower mountain band separately.

### Why the previous fix did not catch it

- [platform_background()](../tools/pce/build_assets.py) chooses `(ly.used_w if ly.extra == 1 else ly.w) * bank.tw`. Level 1 has `w=used_w=61`, `tw=16`, giving 976 pixels.
- [levl.py](../tools/saturn/levl.py) derives `used_w` from nonzero **map entries**, not from the opacity of the tiles those entries resolve to. Nonzero entries can therefore preserve columns whose rendered pixels are all transparent. `load_dump()` also trusts the exported `used_w`.
- `native_background()` serializes that full width. The level-1 sky record consequently has a 122-column period instead of the intended 64 columns of visible art.
- [pce_sgx_sky_stream_body()](../src/platform/pce/sky_stream_sgx.c) correctly repeats `source = col % cols`; it cannot know that the supplied `cols` includes unwanted padding.
- [test_sgx_motion.py](../tools/pce/test_sgx_motion.py) and [test_sgx_rendering.py](../tools/pce/test_sgx_rendering.py) compare live BAT/pattern data against the generated archive. Correct rendering of an incorrectly baked archive passes those checks.

The earlier camera synchronization repair is still relevant. The remaining defect is in the content and period passed to that renderer.

### Saturn reference: preserve its source-aware behavior

[Saturn's layer baker](../tools/saturn/layers.py) and `levl.py` are useful precedents for decoding source tiles and distinguishing map extents from usable art. However, the inspected Saturn level-1 plan gives the upper SkyBG a **static** band at rate 0.0; stage 3 uses its dedicated night sky and moon. The inspected code does not demonstrate an opacity crop to 512 pixels for level 1. Its static presentation avoids panning through the padded tail. Do not claim that copying its `used_w` calculation alone fixes the scrolling SGX sky.

### Required asset change

1. In the SGX SkyBG path of `platform_background()`, render to RGBA before palette fitting and find the meaningful horizontal repeat extent from resolved pixel coverage. For level 1, retain x=[0,512), giving 64 native 8-pixel columns.
2. Restrict this crop to panoramas explicitly intended to repeat. Remove only trailing padding; preserve the left origin, vertical placement, internal transparent gaps, and intentional opaque black pixels. Do not use an unrestricted image bounding-box crop that also moves the horizon or planet.
3. Round the retained right edge up to an 8-pixel tile boundary. If alignment requires added pixels, fill them according to the repeat seam rather than introducing a transparent gutter. Level 1 already ends at an aligned 512-pixel boundary.
4. Fit palettes and emit patterns/maps from the cropped image. Store the resulting repeat width in the sky record's existing `cols` field; retain the foreground camera coordinate and continuous BAT coordinate.
5. Emit source width, retained bounds, repeat width, alpha coverage, and the crop reason into the asset report. Audit stage 3's override and stage 4's bands independently; do not impose the level-1 width on every scene.

At the current level-1 ratio of 64/256, sky offset 512 occurs at camera x=2048. That is a useful boundary fixture: the old archive enters its blank tail there, while the corrected archive returns to source x=0. The BAT continues moving normally across the same boundary.

### Acceptance

- Independently assert that the current level-1 source has 512 pixels of horizontal art and that the baked repeat period is 64 columns. The expected width must come from source coverage or an explicit reviewed source-region fixture, rather than from the baked record under test.
- Capture native frames immediately before, at, and after the 512-pixel sky boundary, including partial 8-pixel offsets, reverse movement, and a second repetition.
- Compare decoded source art, encoded reconstruction, and native composition. No 464-pixel flat interval may appear between repetitions.
- Inspect the join itself: removing padding does not guarantee the original left and right edges are artistically seamless. If needed, author a deliberate seam repair while preserving the planet and cloud features.
- Retain the existing fixed-camera, camera-follow, BAT-wrap, cache-lifetime, horse-residency, and boss-firing checks.

## 2. Title: improve the existing two-VDC implementation

The title already has a paired BG conversion. [frontend.bake()](../tools/pce/frontend.py) calls `sgx_paired_background(title.preview_source, ...)`, and [pce_sgx_ui_load_body()](../src/platform/pce/sgx_pce.c) loads `pce_sgx_ui_pair[19]` for screen 0. This enhancement must improve that active path.

The current [static asset report](../build/sgx/sgx_static_report.json) records **42 distinct BG colors**, 820 BG0 tiles, 45 BG1 tiles, and mean squared fitting error 4.12334 for the title. These counts describe the paired asset, excluding runtime menu and patch sprites. A small fitting error against the current input does not establish fidelity to the original full-color artwork.

### Required changes

- Build the SGX title input from the highest-quality existing source art. [menu.c](../src/menu.c) uses backdrop `0xD7DEBAC0`, logo `0xB04BAC5F`, and shadow `0x989121EC`, with the half-width backdrop mirrored on the right. The current PCE `title_backdrop()` substitutes a synthetic, already dithered blue glow. Inspect the original backdrop and reproduce its framing in the SGX bake.
- Supply full RGB artwork to the palette fit, then quantize to the VCE lattice. Avoid locking in the PCE backdrop's ordered dithering before the SGX fit. Preserve logo gradients, gold/red lettering, blue rays, metal shading, and shadow detail.
- Continue jointly fitting both planes against one shared BG palette allocation. Complementary index-zero masks let each 8x8 tile pair use two palette choices. Measure palette use and visible coverage on each plane; tune fitting where logo detail actually needs it.
- Revisit `sprite_patches()` and `title_draw()`. The existing patches are selected against the single-plane PCE reconstruction, then drawn over the SGX pair too. For SGX, regenerate patches against the final paired reconstruction, or omit patches that worsen or no longer improve it. Keep menu lettering/highlighting intact.
- Report color counts and reconstruction error for the source, PCE baseline, SGX pair, and final pair-plus-sprites. Compare equivalent framing and include detail crops. Set the target from the reviewed source image; a bigger color count alone is not an acceptance criterion.

Both VDCs feed one VCE through the VPC, so their BG palettes must be coordinated. There is no independent second BG color bank or automatic blending. The emulator author's [VPC documentation](https://github.com/drhelius/Geargrafx/blob/main/platforms/shared/desktop/mcp/resources/hardware/huc6202_vpc.md) describes the selected pixel stream sent to the VCE. The existing complementary-mask encoder is the project precedent for richer static art within that constraint.

Acceptance: both planes contribute to the native title; the logo and backdrop visibly retain more source detail/color than the current result; menu highlights remain correct; fades and return from options/game over restore the same composition.

## 3. Character select: separate the fancy backdrop from portraits

Character select currently follows the PCE path. Screen 1 is excluded from the paired-screen branch in `pce_sgx_ui_load_body()`. [select_screen()](../tools/pce/frontend.py) first makes a synthetic tunnel, then replaces complete 8x8 cells with panel/silhouette cells. The chosen portrait is subsequently drawn as up to 50 sprite pieces by [frontend_pce.c](../src/platform/pce/frontend_pce.c).

This arrangement couples backdrop coverage and panel colors within a single BG tile. A separate plane allows genuine transparent panel/portrait edges to reveal the fancy backdrop instead of replacing its tile contents. The generated [selection preview](../build/sgx/preview/ui_select.png) shows the base silhouettes; it does not include the selected portrait's runtime sprites. Intended unselected silhouettes must be distinguished from unintended black rectangles, missing portrait pixels, and stale graphics.

### Proposed SGX composition

Use zero-based VDC numbering throughout this plan; the SDK names the second engine VDC2.

| Surface | Content | Role |
|---|---|---|
| VDC0 BG | Transparent panel frames, intended dim/unselected art, selected color portrait, selection heading | Foreground surface; transparent pixels expose BG1 |
| VDC1 BG | Full-screen fancy spiral and moon backdrop | Independent background surface |
| VDC0 sprites | Names, arrows, cursor/highlight where useful | Small UI elements above both backgrounds |

The portrait belongs to the VDC opposite the fancy backdrop. BG0 is the preferred portrait surface because the project's current VPC setup gives it precedence over BG1. Reuse that validated ordering and verify it in the menu; placing an opaque backdrop on BG0 would hide a BG1 portrait.

### Assets and animation

- Use source spiral `0x92702CF3` and moon `0x2178AD91` from `menu.c::draw_spiral()` as the visual reference. Restore their detail and relative rotation direction rather than treating the existing synthetic tunnel as the finished enhancement.
- Bake a separate transparent foreground map with panel frames and portrait art. Color index zero means a hole; assign opaque black outlines/shading a nonzero index. Preserve any silhouettes intentionally present in the source selection states.
- Allocate the shared 16 BG palettes deliberately across backdrop, panels, and portraits. Keep palette cycling exclusive to backdrop-owned entries. The current `ui_cycle()` overwrites palettes 0-3, and `select_palettes()` overwrites 4-7; SGX must adapt those writes to its actual allocation. A second VDC does not make these palette writes independent.
- The existing portrait palettes live in the sprite half of the VCE allocation. Converting portraits to BG requires a new BG fit and 8x8 planar patterns; the existing sprite palette records and 16x16 pattern data cannot be reused directly.
- Preload all four portraits and their selection states from the UI archive before selection music starts. During switching, stage the next portrait map/patterns and publish a complete foreground state at VBlank. Preserve the old portrait until the replacement is ready, and clear old coverage back to transparent cells.
- For rotating backdrop animation, bake source-derived phases offline, deduplicate their tiles, and measure changed-tile uploads and map updates at the proposed cadence. Start with a complete detailed backdrop and an isolated palette cycle; add the source spiral/moon motion when its transfer budget is demonstrated. Keep the moon in the backdrop bake so it does not require a third independent BG.

### Runtime integration

Add explicit SGX selection records and a screen-1 loader to `frontend.py` and `sgx_pce.c`. The static `PceSgxUiPair` loader provides transfer/layout precedents, but a dynamic selection screen needs its own palette ownership, portrait state, and publication rules.

Update `ui_show()`, selection drawing, cycling, fades, and `ui_end()` together. Match both engines to the current 320x224 front-end view, initialize their scroll/maps while hidden, and enable the complete screen together. On exit, stop backdrop updates and restore both engines, raster state, and VPC configuration for gameplay. The existing VDC1 sprite-enable helper is gameplay-specific; the proposed BG portrait avoids extending gameplay sprite admission into menus.

Budget each engine's BAT, character regions, font/UI reservations, and any inactive map/pattern page. An undeduplicated 40x28 background costs 35,840 pattern bytes plus a 4,096-byte 64x32 BAT. That fits one 64 KiB VRAM, but a second full pattern buffer plus those reservations does not. Use preloaded tile sets and compact map/state changes where possible; do not assume full-screen double buffering is free.

### Acceptance

- All four selected portraits appear complete over the fancy background, with no unintended black boxes, stale old-portrait pixels, or missing pieces.
- Capture every hero selected/unselected and during switching, at multiple backdrop phases. Rapid left/right input and confirmation during an update must publish complete states.
- Backdrop cycling/animation leaves portrait, frame, heading, and name colors unchanged. Portrait selection leaves the backdrop unchanged except for its intended animation.
- Native screenshots match decoded two-plane previews. Validate transparent holes and genuinely opaque black pixels separately.
- Selection confirmation, loading, game over, title/options return, and gameplay transitions restore both VDCs correctly; selection audio remains stable.

## 4. Implementation order and verification

1. Fix and independently verify the level-1 source crop/repeat period.
2. Improve the title input and paired fit, including its sprite patches.
3. Add the separate selection backdrop/portrait assets and loader, then measured animation.

Extend the SGX tests with source-content and retail front-end assertions. `test-sgx` currently covers campaign, motion, rendering, cache pressure, and horses; it needs explicit title/selection image checks. The generic PCE presentation harness uses different debug flows and should not be treated as sufficient SGX front-end evidence.

After implementation, from `game/`:

```sh
make -f Makefile.pce SGX=1 OUT=build/sgx all
make -f Makefile.pce test-sgx
make -f Makefile.pce SGX=0 OUT=build/pce test
```

Run the added SGX front-end checks as part of the SGX target. Record the tested ELF/disc hashes and representative captures. Keep the ordinary PCE behavior and unrelated existing worktree changes intact.

For this document, verification consisted of current-source inspection, source RGBA opacity measurements, generated asset-report inspection, and viewing the sky/title/selection previews. No build, binary test, or physical-hardware test was run, and the fixes above remain to be implemented.
