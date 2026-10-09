# SuperGrafx + Arcade CD-ROM² enhancement plan

Research date: 2026-10-07. Target: the existing `game/` PC Engine port, with a SuperGrafx, CD subsystem and Arcade Card. This is an implementation plan; no renderer changes or hardware experiments were performed for this document. Source observations are distinguished below from proposed designs and prototype gates.

## 1. Intended result and decisions

- All static illustrated screens use both VDC backgrounds at **320×224**: title, hero selection, options/continue panels, game over, victory paintings, credits/ending art and illustrated instruction/briefing cards where applicable. Retain menu animation, text, portraits and overlays.
- Levels 1, 3, 4 and 5 gain a separately scrolling distant background. Restore available foreground art using the additional VDC's sprite system, with correct occlusion and source foreground parallax. Distribute gameplay sprites, including bosses, between the two sprite systems; keep the main player on one fixed VDC.
- Level 6 separates the arena panorama/floor from a new background-based software object renderer, with hardware sprites completing the result.
- Level 7 uses two colorful space backgrounds during ordinary flight, then a persistent space background plus a giant ship background and supplementary pieces. **The boss entrance must not fade the space to black.**
- Keep existing gameplay display widths initially. The requested 320×224 applies to static screens; widening platform gameplay is a separate change with camera, collision visibility, BAT/cache and sprite-cost consequences. Level 2 retains its existing road display and raster until the shared SGX services are stable.
- **Compatibility requirement:** prefer one disc/build that detects SuperGrafx at startup and safely falls back to the existing PC Engine Arcade CD-ROM² renderer and assets when SGX is absent. PC Engine fallback still requires the CD subsystem and Arcade Card. Detection must precede SGX-only RAM allocation, VDC1/VPC initialization and enhanced asset loading; the PCE path must fit the original RAM/layout budgets and remain fully playable. If measured RAM, overlay or asset-layout constraints prevent a combined edition, the shared source must retain both implementations and produce separate PCE and SGX builds with separate generated outputs. Proposed SGX invocation: `make -f Makefile.pce SGX=1 OUT=build/sgx`; this switch does not exist yet. Document the measured reason for separate builds and share gameplay and disc/audio infrastructure.

Three constraints govern the implementation:

1. Two VDCs mean **two background planes total**, plus two sprite systems. A playfield background, sky background and independent foreground background would require three planes. The foreground must therefore mostly use sprites, or a scene-specific background composition technique.
2. The VDCs share the VCE palette. SuperGrafx does not provide native 8bpp background tiles, palette banks per VDC, pixel blending, scaling, or a faster CPU.
3. The VPC does not implement an arbitrary four-layer ordering. Correct foreground occlusion is an early prototype requirement, not a side effect of enabling VDC1.

## 2. Current implementation, traced in the working tree

Paths in this document are relative to `game/` unless prefixed with `../PCE/`.

| Area | Current source evidence | Consequence for SGX |
|---|---|---|
| Display | `src/platform/pce/video_pce.c`: `video_init()`, `timing()`, `video_mode_ui()` | Platform play is 256×224; UI is already 320×224. Both VDCs need matched timings for every mode change. |
| Static assets | `tools/pce/frontend.py`: `Screen`, `paint_cells()`, `Screen.emit()`; `src/platform/pce/ui_pce.c`, `frontend_pce.c`, `victory_pce.c` | Existing records contain one BAT/character set and background/sprite palettes. Add paired backgrounds and one shared palette allocation. |
| World backgrounds | `tools/pce/build_assets.py`: `platform_background()`, `flat_sky_rows()` | Stage 1/3 SkyBG is skipped; stage 4 SkyBG is flattened. Other distant layers are baked into the same world image, losing independent motion. |
| Removed foreground | `build_assets.py`: stage archive emission around the `foreground removed` branch | Levels 1/3 discard the entire extracted foreground. Other stages call `thin_foreground()`. Stage 4 also drops `ForegroundStuf2` before extraction. |
| Retained foreground | `src/platform/pce/foreground_pce.c`, `tools/pce/presentation.py`: `add_foreground()` | Existing foreground is chunked, cached and emitted through the single sprite system. Keep useful extraction/windowing logic but add VDC ownership and layer-specific offsets. |
| Source layers | `build/pce/work/stage{1,3,4,5}.layers`; parser `tools/saturn/levl.py`; original layer construction/export in `src/level.c` and `tools/pce/export.c` | Generated dumps describe the current exported inputs; regenerate and compare before implementation. They also reveal a level-5 scope issue below. |
| Sprite renderer | `video_pce.c`, `sprite_cache_pce.c`, `sprite_generic_pce.c`, `priority_pce.c`, `admission_pce.c`; assembly emit/admission/transfer paths | SAT, caches, occupancy and pending publication currently belong to one VDC. SGX requires two independently accounted destinations and a shared palette owner. |
| Level 6 | `src/platform/pce/m6/m6_c.c`: `m6_blit()`, `arm_draw()`, `big_draw()`; `m6_d.c` | Despite its name, `m6_blit()` currently dispatches **hardware sprite** emitters. A background software blitter is a new subsystem. |
| Level 7 | `src/platform/pce/scenery_pce.c`: `space_hull_load()`, `space_hull_bat()`, `space_hull_draw()`, `space_beam()`; `space_pce.c`: boss phases and `space_frame()` | The current nebula is faded, its single BG storage is replaced by hull art and the surrounding BAT becomes black. Ordinary nebula updates stop when the boss is active. All of these assumptions must change. |
| Test emulator | `tools/pce/emulator.py` | Uses the accurate `pce` core with Arcade Card enabled and `pce.nospritelimit 0`. SGX tests must explicitly enable SGX too. |

Current source foreground status:

- **Levels 1/3:** both `ForegroundStuff` (rate 1.0) and `ForegroundStuf2` (rate 1.2) are tilemaps in the exported input. Restore both, including the night tint on level 3.
- **Level 4:** cabin/wall foreground remains, but the rate-1.2 plant layer is dropped and remaining art is thinned. Restore the full wall layer and plants.
- **Level 5:** `ForegroundStuff` and `ForegroundStuf2` currently have `map 0`; neither supplies an extracted foreground tilemap. There is no populated layer to restore simply by removing the PCE thinning branch. Audit original assets/history and PC/Saturn captures. If no older populated layer exists, new foreground art requires an explicit asset-authoring task; do not claim it has been restored. The distant sky/mountains and extra sprite capacity can still be implemented immediately.

## 3. Emulator-source findings and hardware implications

Use **VDC0** for the original VDC, **VDC1** for the added VDC. SDK/emulator comments sometimes call these VDC1/VDC2 instead.

### 3.1 Sources examined

| Local emulator source | Specific behavior checked |
|---|---|
| `../PCE/mednafenPceDev-main/mednafen/src/pce/vce.cpp`: constructor, `IRQChangeCheck()`, SGX pixel mixer, register read/write methods | Two VDC instances, shared palette lookup, VPC composition, window handling, indexed ports and ORed IRQ1. This is the accurate core used by the project. |
| `../PCE/mednafenPceDev-main/mednafen/include/mednafen/hw_video/huc6270/vdc.cpp`: `FetchSpriteData()`, `RunSATDMA()`, `HDS_Start()`, `TimeFromHDSStartToBYRLatch()`, `TimeFromBYRLatchToBXRLatch()` | Accurate-core 16-unit admission, two fetches for a 32-pixel sprite, separate scroll latch events, pending-write handling at SAT DMA completion and sprite-fetch truncation when horizontal timing leaves insufficient clocks. |
| `../PCE/mednafenPceDev-main/mednafen/src/pce_fast/vdc.cpp`: `FixPCache()`, `DrawSprites()`, `MixVPC()`, `VDC_Init()`; `vdc.h`: `VDC_Read()`; `vpc_mix_inner.inc` | Corroborating palette transparency and per-VDC sprite admission/mixing implementation. The fast core explicitly counts 16-pixel sprite units, with a 32-pixel sprite costing two. |
| `../PCE/mednafenPceDev-main/mednafen/src/pce/pce.cpp` and `pce_fast/pce.cpp`: `DetectSGXCD()`, force-SGX setting | SGX can be forced for a CD image; CD autodetection also recognizes a specific marker in sector 1 of a data track. |
| `../PCE/Geargrafx/src/huc6202.cpp`: `CalculatePriorityMode()`, `CalculateSourceSelection()`; `huc6202_inline.h` | Independent implementation of the priority truth table, two window boundaries, CPU VDC selection and combined IRQ line. |
| `../PCE/Geargrafx/src/huc6260_inline.h` | Both VDC outputs reach one color encoder; no palette concatenation or pixel arithmetic. |

Primary online references checked: [Mednafen accurate PCE documentation](https://mednafen.github.io/documentation/pce.html), [Mednafen fast PCE documentation](https://mednafen.github.io/documentation/pce_fast.html), and [Geargrafx VPC reference maintained with the emulator](https://github.com/drhelius/Geargrafx/blob/main/platforms/shared/desktop/mcp/resources/hardware/huc6202_vpc.md). Local source is the implementation evidence for this repository's emulator; physical hardware verification remains a separate gate.

### 3.2 Resources and register behavior

- Each VDC owns 64 KiB of VRAM (`0x8000` **words**) and a 64-entry SAT. VRAM addresses on one VDC do not refer to the other's storage.
- Each VDC has its own scanline sprite budget: 16 units of 16 pixels, or 256 fetched sprite pixels; 32-pixel pieces consume two units. SGX provides two budgets, not a single pool that automatically accepts 32 units. Transparent portions of admitted pieces still consume fetch capacity.
- The accurate core also models a fetch-time constraint: `FetchSpriteData()` budgets four VDC clocks per admitted unit, and `HDS_Start()` warns if the fetch counter is still positive. Horizontal timing must provide enough fetch time on both VDCs; satisfying the occupancy limit alone is not a timing proof. BYR and BXR latch at distinct horizontal events, so raster probes should timestamp both writes.
- Both VDCs output palette indices to one VCE: 16 background subpalettes and 16 sprite subpalettes, each with 16 entries, from the 512-color RGB333 lattice. Neither subpalette allocation is duplicated by SGX.
- With the project's I/O mapping, original VDC ports are `$0000/$0002/$0003`, added VDC ports `$0010/$0012/$0013`, VPC priority `$0008/$0009`, window boundaries `$000A–$000D`, CPU immediate-VDC selection `$000E`. Explicit port writes and the CPU's immediate VDC instructions have different routing rules.
- IRQ1 combines both VDC sources. Reading the original VDC status alone cannot acknowledge a pending added-VDC IRQ. Start with only VDC0's VBlank/raster IRQs enabled and use that clock to service both displays. Audit the BIOS callback path before enabling any VDC1 IRQ source.
- VPC windows select a priority rule on either side of horizontal boundaries. They are not arbitrary object masks and do not add another background plane.

### 3.3 Priority rules: the foreground problem

The useful both-enabled VPC nibbles are `0x3` (default), `0x7` (mode 1), and `0xB` (mode 2). `0xF` selects default behavior in these sources; it is not a fourth useful ordering. Repeat a rule in all four window nibbles initially, e.g. both priority bytes `0x77` for mode 1.

The VPC sees **one already composed output from each VDC**, with a sprite/background classification and transparency. Each VDC's own sprite/background priority and SAT ordering still matter.

For high-priority sprites, mode 1 gives the useful apparent order:

`SPR0 > SPR1 > BG0 > BG1` (transparent pixels fall through).

This works for BG0 playfield, BG1 sky and ordinary actors on both sprite engines. However, foreground on SPR1 cannot hide a hero on SPR0. Default mode instead allows opaque BG0 to hide SPR1. Mode 2 can put an opaque BG1 above SPR0, which is unsuitable for an opaque sky. No simple rule gives `foreground SPR1 > actors SPR0 > playfield BG0 > sky BG1`.

**Proposed baseline solution:** mode 1; main player/HUD on VDC0; most movable actors and all foreground on VDC1. Give VDC1 foreground earlier SAT entries than VDC1 actors. For each VDC0 actor that must be hidden by foreground, mirror only the overlapping foreground pieces onto VDC0 ahead of that actor in SAT order. The overlapping pieces use identical screen coordinates, patterns and shared palette indices. The hero remains entirely on VDC0. Preserve the HUD's own top ordering.

This mirrors a small subset of foreground instead of placing the entire foreground back in the original scanline budget. Precompute opaque bounds/masks so empty pieces are excluded. Reserve the required mirror units before optional effects. Measure dense foreground plus boss cases: a broad VDC0 boss can make this strategy expensive. Prefer assigning such a boss wholly to VDC1; split boss pieces across VDCs only with explicit occlusion handling.

If the mirror budget fails, prototype actor-pattern masking against foreground coverage or scene-specific tile composition. These consume CPU/VRAM and require their own measurements. Do not silently accept actors showing through walls as an SGX enhancement. This is the principal feasibility gate for full foreground restoration plus reduced flicker.

## 4. Static screens: paired backgrounds and the 256-color target

### 4.1 What paired backgrounds can provide

Both VDC backgrounds remain 4bpp, with one subpalette selected per 8×8 tile. Align both maps to the same 320×224 coordinates. One plane holds a base set of pixels, the other holds complementary pixels with index-zero transparency. Fit their palettes **jointly**, because both use the same 16 background subpalettes. No color is produced by adding the two pixel indices.

A pair of different subpalettes permits up to 30 nonzero colors in an aligned tile pair, plus the common backdrop where both planes are transparent. Across the whole screen, backgrounds with a fixed palette expose at most **240 nonzero BG entries plus one common backdrop: 241 distinct colors**, not 256 independently addressable opaque BG colors. Distinct RGB333 colors may be fewer after quantization/duplicate removal.

Therefore implement the user's 256-color visual target as:

1. A joint target palette of up to 256 distinct RGB333 colors for the complete illustrated screen.
2. Both BG planes carrying the bulk of the painting, with complementary pixel masks and per-tile palette assignments.
3. Sparse, static sprite patches using sprite palette entries for image colors that do not fit the BG allocation. Existing title patches/menu lettering/portraits are precedents, but the SGX converter must plan their actual scanline costs.
4. Report actual distinct visible colors and quantization error for every screen. A low-color source need not be artificially inflated to 256 colors.

If the requirement is strictly **256 colors from backgrounds alone**, a fixed-palette dual-BG implementation cannot satisfy it. A raster palette schedule can increase total colors across different scanline bands, but it is a different technique with palette write deadlines and restrictions. Keep that as an optional measured experiment; do not describe paired 4bpp planes as native 8bpp.

### 4.2 Asset/runtime work

- Extend `tools/pce/frontend.py` or introduce `tools/pce/sgx_static.py` with a paired-plane quantizer. Optimize shared palettes and which pixels belong to each plane together; two independent quantizers would conflict in VCE RAM.
- Produce plane-specific pattern blobs, BATs, complementary alpha/index masks, common BG palette, sprite accent records and palette-animation ownership. Emit reconstructed previews from encoded data.
- Start with one victory painting, then title/game over, then selection/options/continue/credits/cards. Inventory illustrated screens and cut-ins so none are missed. Preserve the currently used art and framing.
- A worst-case 40×28 image contains 1,120 unique tiles per plane: 35,840 bytes of characters. A 64×32 BAT costs 4,096 bytes. Each plane fits comfortably in its own 64 KiB VRAM before font/sprite reservations; assert the complete layout rather than relying on deduplication.
- Load with both planes hidden; publish timings, scroll, palette and both maps together. Normal scene fades can remain. Animated palette writes must update one shared allocation without recoloring the other plane or text.
- Port `ui_show()`, `ui_end()`, fade/restore, portrait switching, victory and game-over paths. Returning to gameplay must restore both layouts and VPC priority, not merely VDC0.

Acceptance: encoded preview matches emulator composition; all screens are 320×224; both BGs visibly contribute; palette count/error is reported; text/menu states and portrait switches remain intact; no accent-sprite scanline overflow.

## 5. Levels 1, 3, 4 and 5: sky, foreground and sprites

### 5.1 Background allocation and parallax

| Stage | VDC0 BG | VDC1 BG | Source motion to preserve or introduce |
|---|---|---|---|
| 1 | Playfield and compatible near scenery; transparent holes toward sky | Restored source sky/distant scene | Exported SkyBG is static (`parallax 0`, `extra 1`). Keep that source appearance first; add a deliberately slow camera-driven sky drift for the requested enhancement, measured against PC/Saturn captures. |
| 3 | Night playfield/near scenery | Night sky/distant scene | Same static source SkyBG; apply the existing per-layer night tint and a chosen slow SGX drift. |
| 4 | Playfield/near scenery and cabin structures behind actors | Original detailed forest sky rather than flattened rows | SkyBG source rate 0.03. Other exported distant rates are 0.10, 0.20 and 0.32; preserve extra rates only where compatible with two-plane composition. |
| 5 | Lab playfield/MidBG and compatible near scenery | Original distant sky/cave vista | SkyBG rate 0.20; Mountains rate 0.50. Restore separate motion where the two surfaces can be represented faithfully. |

**Independent sky parallax is feasible:** VDC1 has its own BXR/BYR. Compute source-compatible fixed-point camera offsets and publish them with the SAT/scroll frame. A 0.03 rate needs a fractional accumulator; integer division per camera step would stall it. Match the PC's rounding, static/time-driven `extra` behavior, map extents and repeat/clamp rules using `tools/saturn/levl.py` and the Saturn plane implementation as references.

Additional cloud/mountain bands with distinct rates are possible through VDC1 raster scroll when bands are vertically separated. Overlapping silhouettes cannot all retain independent rates on one BG just by changing BXR per scanline. Initially select one coherent distant panorama or merge compatible rates; only add bands with a demonstrated composition plan. Full Saturn eight-layer motion is not implied by SGX.

The current platform bake initializes an opaque black image. Removing the sky without preserving alpha would leave black BG0 pixels hiding BG1. Generate explicit index-zero holes in BG0 wherever the distant plane must show. Keep genuinely opaque black art distinguishable from transparent pixels.

Reserve a VDC1 sky character area and BAT ring, stream newly exposed columns from Arcade RAM, and independently retire held cache slots after the displayed frame has changed. A sky-plus-foreground sprite layout can start with BG/BAT space comparable to VDC0, but derive its final partition from measured restored art.

### 5.2 Foreground restoration

- Bypass PCE-only removal/thinning for SGX assets; preserve those branches for the PCE edition. Restore the source layer before any filtering so SGX does not inherit permanently discarded pixels.
- Store rate-1.0 and rate-1.2 foreground as separate layer records. The current single baked foreground image is not sufficient to recover independent rate-1.2 motion reliably.
- Reuse `add_foreground()` chunk deduplication and `foreground_pce.c` world-window logic, but compute each layer's source-compatible camera offset and assign its patterns/SAT to VDC1.
- Preload visible/near-visible chunks; retain them while still referenced by the displayed SAT. Restore full stage-4 walls and plants, not just the current thinned output.
- Implement the VDC0 mirror subset described in section 3.3. Cover standing/jumping player, camera shake, both facings, foreground transitions, dialogue actors and bosses.
- Do not charge foreground directly to VDC0's admission array except for required mirror pieces. Do charge its entire cost to VDC1: adding the VDC does not make foreground free.

### 5.3 Gameplay sprite allocation and flicker reduction

Separate **asset residency** from **draw assignment**:

- Patterns must be present in the destination VDC's VRAM. Frequently migratable enemy/projectile/boss sets can be cached on both VDCs. Copy the patterns twice only when useful; one Arcade-RAM copy is enough as an upload source.
- Draw a gameplay piece once, on its assigned VDC. Drawing every sprite on both engines doubles work without reducing overflow. Foreground occlusion mirrors are a deliberate exception.
- Shared sprite palette indices must mean the same colors on both engines. The current cache associates palette selection with a slot (`slot < 15 ? slot : 15`); two independent slot allocators cannot keep this convention independently. Add a shared palette allocator or stable asset-family assignments.
- Keep the hero and its normal animation on VDC0 for every frame. Decide attached effects/projectiles explicitly; do not migrate the hero to solve admission.
- Use deterministic, sticky actor ownership to avoid needless uploads. Reserve hero/HUD/required foreground mirrors first on VDC0, foreground and essential VDC1 actors on VDC1, then place remaining actors according to **both** per-line occupancy and SAT/cache availability. Optional effects come last.
- Admission is transactional: reserve all required pieces of an actor/pose, or roll back all units and entries on failure. A boss can be assigned wholly to one VDC or split into stable pieces if overlap ordering and mirroring have been proven. Prefer whole-boss ownership when it fits.
- VPC mode 1 makes SPR0 win over SPR1 where they overlap. It cannot reproduce arbitrary depth ordering between actors assigned to different engines. Group intersecting actors by required order, assign stable families where possible, and validate crossings. Preserve VDC-local sprite BG priority flags rather than forcing every object to the front.
- Instrument refused actors separately from hardware overflow. Existing gameplay protections for invisible harmful projectiles must apply regardless of which VDC refused the draw.

Acceptance: all available source foreground appears at its proper rate; main player stays on VDC0; restored foreground hides actors correctly; no missing boss pieces in the prescribed stress captures; no essential admission failures in those fixtures. Record per-engine peaks and refused cosmetic effects. **Universal absence of flicker is not guaranteed merely by doubling VDCs**; demonstrated worst cases determine whether re-slicing or additional adaptations are needed.

## 6. Level 6: panorama/floor plus software objects

Proposed allocation:

| Surface | Contents |
|---|---|
| VDC0 BG | Arena panorama and floor texture, with the existing floor raster refined after profiling |
| VDC1 BG | Transparent tiled software object surface: nearest mech silhouettes, large arm sections and expensive large effects |
| VDC0/1 sprites | Smaller distant objects, cockpit/HUD, bolts, highlights and details selected to satisfy depth and scanline budgets |

Keep the panorama as a preloaded/static asset; scroll it with heading if needed. “Static background” does not require freezing camera motion. The floor can retain raster BXR/BYR on VDC0 while the software object surface stays independent on VDC1.

Implementation sequence:

1. Preserve `m6_c.c`/`m6_d.c` simulation, scale ladders and object transforms. Reuse current baked nearest-pose assets as inputs, but generate BG-format 8×8 planar tiles and coverage masks. Existing sprite-format patterns are not interchangeable with BG patterns.
2. Prefer pre-baked tiled poses and maps for large objects. Add a software dirty-tile compositor only where overlapping objects or sub-tile positioning require it. This minimizes CPU work compared with redrawing a full bitmap every frame.
3. Maintain dirty coverage from both old and new positions; clear exposed old pixels to index zero, not to a copied floor. Compose objects in depth order and preserve transparent edges.
4. Begin with tile-aligned positioning plus a single plane scroll offset for one dominant object. Multiple independently moving objects require pre-shifted patterns or compositing; BXR/BYR alone moves the whole surface.
5. Size and reserve active/inactive pattern regions and map pages. Publish only complete tile/map updates. A 256×224 4bpp surface costs 28,672 bytes of patterns before metadata; two full pattern buffers cost 57,344 bytes, leaving limited VRAM for BATs and sprites. Do not assume two full software surfaces plus a large sprite cache fit.
6. Evaluate single dirty surface with deferred publication, compact object-local pages, or restricted double buffers. CPU-side masks, dirty lists and temporary tile data also need console-RAM budgets.
7. Set VPC priority deliberately: with BG1 objects over BG0 floor, default BG0 dominance would hide the objects. Options include VDC0 sprite silhouettes hiding BG0 via its own local priority before VPC selection, or swapping **scene roles** so the software object BG is BG0 and the panorama/floor is BG1. Prefer the role swap for the prototype; retarget floor raster writes explicitly. The fixed player/HUD sprite VDC can remain VDC0.
8. Partition hybrid sprites by required depth. With software objects on BG0 and floor on BG1, mode 1 allows sprites on either engine above those objects. Art meant behind a software silhouette must use an appropriate VDC-local priority/placement or be composed into the object surface. Do not assume per-object Z can be expressed by the VPC.

The second BG removes competition between floor/panorama characters and software object pixels. It does **not** remove HuC6280 composition or upload cost. Measure uploaded bytes, dirty tiles, audio IRQ latency, raster deadlines and frame cadence with the nearest mech plus arm and multiple shots before converting the full arena.

Acceptance: floor retains its motion with no torn bands; large mech/arm stay complete; no stale silhouettes; hybrid depth is correct; audio remains stable. Use current hardware-sprite large-object rendering as a measured baseline and retain a fallback until the software path demonstrates an improvement.

## 7. Level 7: two-layer space and the giant ship

### 7.1 Ordinary flight

- Use BG0 for transparent nearer stars/nebula detail and BG1 for a richer distant nebula, or another explicitly validated near/far assignment.
- Bake complementary coverage and jointly allocate BG palettes. Give the layers different scroll rates with repeat-safe maps; do not duplicate the same opaque image on top.
- Distribute fighters, mines, projectiles and explosions using the shared sprite allocator. Retain the player on its fixed VDC.

### 7.2 Boss entrance and battle

Two independent space BGs plus an independently moving ship BG would require three planes. **At the entrance, one space plane changes role to the ship; the other remains visible throughout.** Preserve apparent richness with extra star sprites, a baked composite surviving space layer, palette animation, or vertically separated raster bands where compatible.

Concrete initial design: retain the space panorama on BG0, stage the ship on BG1, use the VPC to display the hull in front where needed. Because a nontransparent BG0 normally wins over BG1 in the useful sprite-spill mode, this allocation requires a priority/composition proof. Prefer the simpler final role assignment **BG0 = transparent ship, BG1 = persistent space** during battle. Before the role change, preload a persistent copy/composite of the space on BG1; publish the change without blanking both VDCs. This avoids per-frame masking of the space under the moving hull.

- Create an SGX `space_hull_load()` path that performs no nebula-to-black fade and no blanket `video_display(false)` on the shared scene.
- Stage hull assets in unused VRAM or upload incrementally to the hidden destination while the surviving space plane and sprites continue. All battle assets should already be in Arcade RAM; retain the current no-disc-read boss transition.
- Build a transparent hull BAT, synchronize the role/priority/scroll change in VBlank, and continue ticking the surviving space scroll during `space_frame()` even with a boss active.
- Keep hull and space palette slots disjoint within the shared BG half. Hull-hit whitening must change only hull colors. Audit dialogue white, laser palette 4, whole-screen flashes, power cut-ins, pause and restore paths.
- Route `space_beam()` to the hull surface or use hybrid sprites. Preserve collision/beam coordinates independently of which VDC displays them.

### 7.3 “Other VDC plus a bit of the first”

Use the second background's dedicated storage for the ship body, and use the surviving space VDC's **sprite system** for hull extensions, guns, engine animation, highlights and explosions. This supplies the requested contribution from both VDCs while retaining an independently scrolling space backdrop. Actual VDC numbers can change with the battle role swap above.

If part of the hull must specifically occupy the surviving **background**, its pixels must be composited into that space tilemap and restored as the hull moves. That is a software dirty-tile renderer with additional scroll coupling; treat it as a separate prototype, not a free third layer.

The current `boss_background()` in `tools/pce/build_assets.py` resizes the source's 224×123 hull to 180×98, and runtime `space_hull_bat()` uses a 23×13 tile footprint. Inspect the original `boss.png`/`boss.txt` and PC/Saturn framing, then choose a larger SGX hull composition, with the source size as the first comparison candidate. More VRAM enables a larger tile set but does not by itself recover original scale. If visual scale changes, update `space_hull_top/bottom`, gun ports, muzzle/beam positions and contact tests from the same transform; keep rendering and collisions synchronized.

Acceptance: no black backdrop during entry, dialogue, hit flashes or death; hull remains giant and complete; supplementary pieces align at all hull offsets; space keeps moving behind it; ship destruction restores the two-space-plane flight layout or transitions cleanly to the ending.

## 8. Shared driver, memory and integration work

### 8.1 Detection and initialization

The pinned SDK exposes `pce_sgx_detect()`, `pce_sgx_vdc_init()`, VDC selection and index/data getters in `../PCE/llvm-mos8/mos-platform/pce-common/include/pce/vdc.h`. Use these as bring-up references, but audit their linked implementations before adoption: the current game bypasses SDK selection in many explicit port writes.

- Detect SGX while display is hidden/VBlank as the SDK recommends, before touching added hardware or RAM. Keep Arcade Card detection/load requirements intact. For a combined edition, select the original PCE renderer, assets and memory layout when detection fails. If separate builds are necessary under the compatibility requirement, the PCE build must remain fully playable and the SGX build must present an intelligible requirement screen on ordinary PCE rather than accessing aliased ports blindly.
- Initialize and clear both VDCs, synchronize timings, set explicit VPC priority and neutral windows, and keep immediate-VDC selection predictable.
- Audit `video_vdc()`, `arcade_vram()`, SAT copies, fast sprite emission, raster handlers and BIOS software shadows (`$20F7` and related state). Switching an SDK pointer alone does not retarget hard-coded writes.
- Maintain separate VDC index shadows. Keep VDC transactions atomic across an IRQ; preserve the immediate destination and active register as required. Add destination-aware transfer helpers and short burst scheduling for each VDC.

### 8.2 Ownership and publication

Introduce an explicit per-VDC state structure for SAT count/pages/VRAM sources, occupancy, pending status, scroll and pattern caches. Names and storage locations are implementation choices, not existing APIs.

- Two SAT tables must be complete before publication. Commit the paired frame state in one VBlank epoch; hold both displayed scrolls if one half is not ready.
- Extend pause/retirement/dialogue/direct-SAT replacement paths to both engines. Existing `sat[1]` is scratch, not a spare second-engine table.
- Two engines with two CPU SAT pages each require 2,048 bytes versus the current 1,024 bytes, before other metadata. Two 224-line occupancy arrays cost 448 bytes. Budget this explicitly.
- Shared VCE allocation owns fades, flashes and animation. Per-VDC caches own VRAM words, residency and held references. Arcade directories must distinguish VDC and asset namespace.
- Enlarge telemetry without breaking the existing `SRPC` test ABI: version it or add a separate SGX block. Record SAT counts, scanline peaks, essential/cosmetic refusals, residency misses, per-engine uploads, mirror cost and paired-publication stalls.

### 8.3 RAM, overlays and Arcade Card

The existing linker uses mapped CD-RAM overlay banks and reserves compiler/audio zero page. Do not insert a second renderer into nearly full banks. The existing generated `build/pce/runtime.json` reports bank 106 at 8,153/8,192 bytes, bank 108 at 8,166/8,192 and bank 119 at 8,190/8,192. These are a **snapshot of existing artifacts**, not a newly built SGX memory measurement.

SuperGrafx has additional base RAM; the local emulator maps banks `$F8–$FB` distinctly in SGX mode. Establish an SGX-specific linker/runtime map before allocating that space. Existing SDK/CD startup conventions and MPR3/6 overlay/transfer ownership still apply; additional physical RAM does not automatically enlarge the compiler's current sections.

Keep interrupt code, audio state, live publication state and return paths resident. Add overlays for conversion-independent SGX services if needed, and expand `tools/pce/check_elf.py` to check both per-scene VRAM layouts, shared RAM, linker bounds and reserved regions. Recalculate Arcade archive sizes, staging regions and directory allocation against the existing 2 MiB Arcade Card; duplicate VRAM copies do not require duplicate source archives.

## 9. Ordered implementation milestones

| Milestone | Deliverable | Gate before proceeding |
|---|---|---|
| M0: source/asset inventory | Re-export layers, enumerate static screens, find stage-5 foreground provenance; baseline captures and profiles | Every requested enhancement mapped to a current asset/runtime owner; missing source art identified |
| M1: SGX bring-up | Safe detection/fallback, combined-build memory feasibility audit, two timed VDCs, VPC test patterns, dual VRAM/SAT upload, shared palette and IRQ handling | Ordinary PCE boots the original path safely; CD + Arcade Card + PSG/ADPCM/CD-DA still work; clear priority/transparency truth-table captures; separate builds only if constraints are demonstrated |
| M2: critical feasibility probes | One paired victory painting; dense restored foreground with hero occlusion mirrors; nearest mech BG prototype; persistent-space hull entrance | Color target accounting, foreground budgets, software upload cadence and hull priority all demonstrated |
| M3: full static-screen conversion | All illustrated UI/victory/game-over/card assets and transitions | 320×224, paired-plane contribution, usable UI and clean restores |
| M4: platform restoration | L1 then L3, L4 and L5 distant planes; full available foreground; stable dual-engine gameplay/boss admission | Whole-level camera sweeps and stress fixtures pass with per-engine limits enabled |
| M5: arena renderer | Background software objects plus hybrid sprites and retained/refined floor | Measured improvement over current renderer; no regression in frame/audio/raster behavior |
| M6: space finale | Two-space-plane flight, seamless role change, larger transparent hull and supplementary pieces | Continuous colorful space, correct beam/contact/death/restore behavior |
| M7: campaign/release | SGX and original PCE builds, documentation, disc packaging, emulator and hardware checks | Full campaign and all transition regressions pass; SGX setup clearly identified |

Do not estimate a completion date before M2: foreground priority and arena software transfer costs are the major unknowns. Asset recovery/authoring for level 5 is independent of the driver work.

## 10. Validation plan

### Emulator setup

Extend `tools/pce/emulator.py` with explicit edition selection. For the accurate core use `pce.forcesgx 1`, `pce.arcadecard 1`, `pce.nospritelimit 0`. The fast-core equivalents use `pce_fast.*`; do not feed them to the accurate-core wrapper. Cross-check VPC behavior in Geargrafx with SGX + Arcade Card enabled and hardware sprite limits intact. Never use an emulator's unlimited-sprite option for acceptance. These setting meanings are documented in [Mednafen's PCE documentation](https://mednafen.github.io/documentation/pce.html).

For auto-recognition, both inspected Mednafen CD detectors look for bytes `4D 65 64 6E 61 66 65 6E 74 AB 90 19 42 62 7D E6` at byte offset `0x6A` of data-track sector LBA+1 (logical data-track byte offset `0x86A` with 2,048-byte sectors). Explicit force-SGX is sufficient for prototypes. Only add the marker to `tools/pce/build_disc.py` after auditing IPL/header ownership; it is an emulator convention, not SGX hardware detection or a substitute for Arcade Card checks.

### Focused checks

- **VPC matrix:** opaque/transparent BG and sprite pixels on both engines, sprite-local priority, two overlapping actors, hero behind restored walls, windows neutral and active. Capture each plane/sprite engine independently and the final composite.
- **Static asset checks:** decode emitted planes, validate pixel masks, common palette ownership, per-tile palette constraints, RGB333 counts/error and sprite accent scanline costs. Compare title/game-over/victory screenshots with encoded previews.
- **World sweeps:** every camera column in L1/3/4/5, reverse movement, both hero facings, jump/shake, foreground layer entrances, dialogue opening/closing and restored rate-1.2 plants. Verify VDC-local tile directories and held-slot lifetime.
- **Sprite stress:** existing herd encounters, largest bosses, shots/grenades/explosions plus dense foreground. Record actual hardware overflow and software refusals separately, and verify rollback never creates invisible damage.
- **Arena stress:** nearest mech + arm, overlapping mechs, all dirty edges, cockpit/HUD and audio active, floor raster at maximum upload pressure. Check each publication and raster deadline against accurate-core traces.
- **Space stress:** entry while shots are active, all hull offsets, laser widths, hit/bomb flashes, greeting, power cut-in, pause, death/respawn and breakup. Check surviving nebula pixels and scroll through the entire entrance.
- **Shared regressions:** level 2 road raster/dialogue, campaign stage transitions, saves/continues, CD music, ADPCM/PCM, and return from every UI mode. Adapt PCE-specific assertions such as “foreground count zero on stages 1/3” into edition-specific expectations.
- **Compatibility:** boot the combined disc with SGX disabled and verify a complete campaign through the original renderer, with no SGX-only memory or port accesses after detection. Test SGX-enabled boot separately. If memory constraints require separate editions, build and test both from the same source revision and verify graceful rejection of the SGX edition on ordinary PCE. A Mednafen SGX autodetection marker must not force the combined-disc fallback test into SGX mode; use an explicitly controlled PCE test configuration/image.

Run the existing `make -f Makefile.pce test` on the PCE edition after shared driver changes. Add SGX acceptance targets beside it rather than reinterpreting the PCE-only tests. Profile with the same audio/raster/gameplay workload as the baseline. A second VDC raises throughput only where its independent hardware resources help; CPU and I/O contention still need measurement.

Final evidence should distinguish generated-asset checks, automated emulator tests, manually inspected captures, a full human campaign and physical SuperGrafx + CD + Arcade Card tests. Physical hardware is the final timing/BIOS compatibility gate. None of these execution checks has been run as part of this planning-only task.
