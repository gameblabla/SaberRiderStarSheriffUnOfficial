# Saber Rider: PC Engine Arcade CD-ROM² port plan

Implementation status, build instructions and measured results are tracked in
[PCE_PORT.md](PCE_PORT.md). This document retains the original design and its
acceptance gates; proposed targets below are not claims that those gates passed.

Date: 2026-10-03. Target: an ordinary PC Engine VDC, Super CD-ROM² system memory, and an Arcade Card. This document is a source-based implementation plan; no port or performance benchmark has been built for it. All frame rates and allocation budgets below are proposed targets until measured.

## 1. Recommendation and scope

**Use LLVM-MOS for the C code, the existing PCE CD SDK for boot/BIOS/banking, and a small project-owned Arcade Card driver plus HuC6280 assembly kernels.** Retain HuC/HuCC and PCEAS as reference implementations and optional standalone diagnostic tools. Do not rewrite the whole C11 game into HuC's language subset first.

The installed `/home/anonymous/llvm-mos/` already provides `mos-pce-cd-clang`, `pce-mkcd`, BIOS declarations, Super CD linker scripts, and Arcade Card register definitions. Its compiler reports Clang 24.0.0git, LLVM-MOS revision `9e5efd81a0b7c23e62e8ef90d298f2ddbbb4ed35`. `PCE/llvm-mos8/` also contains the PCE CD SDK. Pin one installation and its headers/libraries together.

The necessary extension is an Arcade Card asset/cache API and explicit memory layout. There is already a CD platform to extend. The upstream SDK also documents `mos-pce-cd-clang`, `binary-scd.ld`, and CD image construction. [LLVM-MOS PCE target documentation](https://www.llvm-mos.org/wiki/PCE_target).

Preserve all six stages, both stage-2 phases, stage 6's space finale, all four heroes, collision rules, stage progression, dialogs, and power-attack results. Adapt art density, parallax, particle counts, numeric precision, and presentation to the hardware. Use CD-DA music; use the CD unit's one ADPCM voice and PSG channels for effects. Replace videos with still artwork, text, palette changes, and short sprite animations. Stage 2 must use the Wolf3D BAT framebuffer technique in both racing and pursuit. Stage 6's mechs must use the supplied coarse-size-plus-slice technique.

An Arcade Card expands storage and reduces disc access. It adds neither a second VDC background nor scaling, blending, a framebuffer processor, or hardware PCM voices to the six PSG channels.

## 2. What the current game actually requires

Paths in this document are relative to the repository root containing `game/` and `PCE/`, except explicitly absolute toolchain paths.

| Existing code/data | Finding | Consequence for the PCE port |
| --- | --- | --- |
| `game/src/platform/render.h` | Texture rectangles, arbitrary scaling/rotation, alpha/additive blend, geometry, retained layers, and perspective floors | Implement an asset-aware PCE renderer and stage-specific draw paths; a generic SDL renderer emulation would be too expensive |
| `game/src/app.c`, `game/src/game.h` | One static `Game` includes many stage systems at once; the source explicitly describes several hundred KB of state | Split resident session state from the active stage; use overlays and unions/arenas |
| `game/src/bullets.h` | 1,024 bullets in each of two pools | Replace oversized arrays with measured active pools; separate collision objects from hardware sprite allocation |
| `game/src/enemies.h`, `effects.h` | 64 enemies, 100 triggers, 128 effects | Pack entities, keep immutable definitions outside active state, and bound cosmetic effects |
| `game/src/real.h`, `fx.h`, `fx.c` | Saturn fixed point is 16.16 with frequent 64-bit multiply/divide and distance calculations | Useful behavioral reference; requires cheaper PCE arithmetic, especially in projection and movement |
| `game/src/pack.c`, `assets.c`, `gfx.c` | Runtime file directories, decompression, PNG/RGBA paths, large caches; `aligned_alloc` appears in `pack.c` | Convert assets and metadata on the host; replace runtime pack/file loading with compact sector/resource tables |
| `game/tools/saturn/layers.py` | Stage 1 already has 11 tile layers at eight scroll rates; the Saturn port merges some | Reuse its extraction and compositing ideas, then create a new one-VDC plan |
| `game/src/mode7.c` | 1,024² byte material map normally, 512² on Saturn; 1,024 track points; seven rivals; 200 entity slots | Store large immutable data in Arcade RAM; cache hot map data and track segments in mapped RAM |
| `game/src/ramrod.c` | Perspective sand floor, panorama, cockpit, up to three active wave mechs; generous prop/shot/effect pools | Rebuild composition around a cockpit viewing window; scale mechs with baked sizes and slice placement |
| `game/src/space.c` | Stage 6 finale is internally stage 7; 96 foes, 220 shots, 320 effects, 110 stars, a tilting cruiser | Include a separate shooter overlay and a deliberate bullet, scenery, and boss renderer |

Existing `game/build/saturn/work/stage3.layers`, `stage4.layers`, and `stage5.layers` are useful inspection artifacts, but regenerate them from current sources when implementing. They are not the PCE asset format. Stage 4's recorded rates include 0.03, 0.10, 0.20, 0.32, 0.48, 0.66, 1.00, and 1.20. This is substantially more than a background plus one sprite layer can preserve exactly.

## 3. Toolchain choice and the required SDK work

### 3.1 LLVM-MOS versus HuC/HuCC

| Choice | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Legacy HuC | Established PCE/CD examples and libraries | Extensive source changes for this game's types, initializers, callbacks, compound literals, and C11/library use | Keep as a reference |
| Current HuCC in `PCE/huc/` | Improved PCE libraries and code generation; useful banked game structure | Still inherits language/layout restrictions; modern C source is not automatically portable to it | Use if intentionally building a substantially rewritten PCE game or a small standalone hardware experiment |
| LLVM-MOS with existing PCE CD SDK | C11 frontend, HuC6280 target, linker control, existing CD APIs, optimization/LTO | Explicit banking and interrupt integration remain necessary; some SDK details need correction | Main implementation |
| LLVM-MOS plus assembly | Keeps C for game flow while controlling raster, upload, audio, and sampling costs | Small assembly ABI and timing surface to maintain | Recommended combination |

The local `PCE/huc/README.md` describes HuCC's limited structure initialization and preference for small arrays and structures of arrays. Upstream reports improvements over HuC, but those figures do not compare HuCC with LLVM-MOS. [HuC/HuCC project documentation](https://github.com/pce-devel/huc).

Do not claim that LLVM-MOS produces faster code for this game before comparing representative kernels. The reasons to select it now are source compatibility and the available CD infrastructure. Later compare a packed entity update, bullet collision, SAT construction, map lookup, and a frame's floor drawing using equivalent algorithms and numeric widths. Record executable bytes, cycles, stack/ZP use, and helper calls; compare HuCC as well as legacy HuC if evaluating alternatives. Hot paths can be replaced with assembly without switching the whole project.

### 3.2 Concrete integration

1. Add `game/Makefile.pce`, a PCE configuration header, and `game/src/platform/pce/`. Start with `mos-pce-cd-clang -std=gnu11 -Os` (C11 plus the GNU assembly/attributes used by the SDK headers); evaluate `-O2` separately. Follow the SDK's LTO configuration and inspect final linked output.
2. Use a small IPL executable and a Super CD application layout based on `binary-scd.ld`. Keep a resident kernel while loading stage overlays into assigned banks. A whole-program `pce_cdb_cd_exec` transition and a partial overlay load have different lifetimes: explicitly preserve session state for either approach.
3. Use existing `pce_cdb_cd_read`, `pce_cdb_cdda_play`, ADPCM BIOS calls, and BIOS interrupt hooks from `/home/anonymous/llvm-mos/mos-platform/pce-cd/include/pce/cd/bios.h`. Build sector references through `pce-mkcd` and emit a CUE with CD-DA tracks. Supply the documented 2,048-byte `ipl.bin` input from an existing suitable disc image.
4. Add `arcade_pce.c/.h` and assembly upload/copy routines. Expose detection, port setup, bounded reads/writes, CD preload, and Arcade RAM-to-VRAM copies. Arcade addresses are offsets/handles, never ordinary C pointers.
5. Keep generated HuC/PCEAS code separate unless its assembly syntax and calling conventions have been explicitly adapted to LLVM-MOS. Matching the CPU does not make object formats, zero-page conventions, parameter passing, or interrupt prologues compatible.
6. Place all IRQ code and the data it needs in stable mappings. Handle BIOS scratch space and LLVM-MOS imaginary registers explicitly; avoid unprotected C calls from an IRQ into interrupted C state. Audit the SDK hook implementation before selecting the save/restore convention.
7. Audit unresolved ELF symbols after disc processing. The installed PCE CD configuration uses `--unresolved-symbols=ignore-all` for its relocation workflow; accidental missing functions must not silently become a successful build.

**Two installed SDK details require attention:**

- `cd-sections.ld` places ordinary `.bss` in the console RAM region. Selecting the Super CD linker script does not automatically give every C global a flat 256 KiB heap. Add explicit mapped working sections and initialization; coordinate their MPR mappings and overlay lifetimes.
- `pce-common/include/pce/hardware.h` defines Arcade access macros using `0x1A00 + page`, and its port-1 convenience macros pass `1`. Actual port blocks are spaced by `0x10`, so the convenience definitions appear to address the wrong registers beyond port 0. `IO_AC_RAM_OFFSET` also exposes only an 8-bit access despite a 16-bit offset register. Implement clear project-owned low/high register accessors, compare against `PCE/huc/examples/asm/elmer/doc/arcade-card.txt` and Mednafen, and check all four ports before relying on those macros.

The driver's simplest initial mode is a 24-bit base, offset disabled, increment 1, base auto-increment enabled (`control = 0x11`). Reserve more complicated addressing modes for measured needs. The local notes and emulator disagree in some descriptions of advanced sign/trigger behavior; ordinary positive sequential transfers avoid those ambiguities.

### 3.3 C portability audit

The PCE ABI has narrower ordinary integers and pointers than the existing PC/Saturn targets. Check the pinned compiler's actual widths before converting structures. In particular, expressions such as `p[2] << 16` and `p[1] << 8` in binary readers need explicit unsigned wide casts before shifts; use 32-bit sector, file-length, map-index, and address arithmetic where required. An individual C object cannot become a megabyte-sized array merely because Arcade RAM exists.

Remove runtime dependency on filesystem-style `FILE` operations, PNG/RGBA expansion, Vorbis/FFmpeg, large `printf` machinery, and an unbounded allocator. Convert all original formats using host tools. Retain compact resource IDs, serialized metadata, and the game logic that operates on active state. Disable runtime floating point; the C11 frontend alone does not make floating point or 64-bit math cheap on a HuC6280.

## 4. Memory, banking, and disc policy

| Resource | Hardware capacity | Intended use |
| --- | --- | --- |
| Base console RAM | 8 KiB, with BIOS/compiler reservations | ZP/stack, resident IRQ state, SAT staging, tiny queues |
| CD + Super CD mapped RAM | 256 KiB total, banks `$68-$87` | Resident code, stage overlay, active entities, sampling tables, small hot caches |
| Arcade RAM | 2 MiB through four data ports | Graphics, scale variants, maps, sample archives, immutable metadata |
| VDC VRAM | 64 KiB | BAT, background characters, sprite patterns, SAT source |
| CD ADPCM RAM | 64 KiB | Current stage's prioritized sample bank |

The four Arcade port windows are at `$1A00/$1A10/$1A20/$1A30`. Physical banks `$40-$43` mirror their data ports and allow ordinary HuC6280 block transfers. This is sequential port access, not direct mapping of the 2 MiB into the CPU address space. The expansion's architecture is also described by the [HuC Arcade Card documentation](https://github.com/pce-devel/huc/wiki/Arcade-Card).

Use a fixed resident mapping for the kernel, preserve base RAM and BIOS visibility, then dedicate the remaining MPR windows to stage code, working data, and a temporary transfer window. Define bank+offset handles and call trampolines. Do not remap a live return address, the soft stack, active C pointers, or IRQ dependencies. The SDK's `PCE_RAM_BANK_AT` declarations in `mos-platform/pce-cd/include/pce/config.h` are the starting point for mapped RAM; use the CD header search order rather than the cartridge version of that header.

Proposed mapped 256 KiB allocation, to revise from link maps:

| Use | KiB |
| --- | ---: |
| Resident kernel + active stage code | 96 |
| Packed active state | 32 |
| Math/projection tables | 32 |
| Hot map/track/collision cache | 32 |
| Graphics/audio staging | 32 |
| Loader/workspace/headroom | 32 |
| Total | 256 |

A proposed stage-2 Arcade allocation is 1,024 KiB for its original material map, 256 KiB for material/auxiliary data, 512 KiB for baked graphics, 128 KiB for sound archive, 64 KiB for metadata, and 64 KiB spare. This totals 2 MiB and must include **both phases'** immediate working sets. If it does not fit, compress/chunk the map or evaluate 512²/coarser material cells with road-edge comparisons. Never read the full map through a C pointer or assume that random port reads per texel will meet the frame budget.

For other stages, allocate Arcade RAM per stage rather than reserving the stage-2 map permanently. A cache manifest must report exact RAM, VRAM, palette, and variant costs before a disc is built.

**Preload before CD-DA starts.** A single optical pickup cannot continuously play music while seeking elsewhere for stage assets. Preload stage graphics, metadata, ADPCM bank, PSG samples, and needed transition art. Stage 2's phase change should be entirely RAM/Arcade-driven. For larger changes, use an explicit loading transition with music stopped and restarted. Boss music changes can seek to another audio track, but must not also trigger lazy asset reads. Disc sector counts, track numbers, and endpoints come from a generated manifest.

Use BIOS RAM reads to fill a staging buffer first. Optimize CD-to-Arcade transfer using mirrored banks only after confirming BIOS bank advancement across boundaries; advance/remap port windows or split transfers deliberately. Similarly split Arcade-to-VRAM transfers at mapped-window boundaries. Ordinary Arcade RAM-to-VRAM copies are CPU block transfers, not independent Arcade DMA.

## 5. Common renderer and asset conversion

### 5.1 Display, colors, and VRAM

Use 256×224 as the initial platform/shooter display. Preserve source pixel sizes where possible and adjust the camera/HUD for the narrower view; recompose 426×240 screens for PCE presentation. A 320-dot option is a later art/budget decision. Do not globally shrink every world coordinate and collision box to fit a screen.

PCE characters are 8×8, 4bpp, 32 bytes each. Sprite storage is 128 bytes per 16×16 pattern; larger hardware sprites use multiple patterns with alignment constraints. Quantize to the VCE's 9-bit RGB palette, with 16 background and 16 sprite subpalettes. Respect transparent sprite index 0 and background color-0/backdrop behavior. Bake flips into character variants when necessary; background BAT entries do not provide arbitrary sprite-style flips.

Example platform-stage VRAM budget in **bytes**, not VDC word addresses:

| Use | KiB |
| --- | ---: |
| 64×32 BAT | 4 |
| Background characters, including foreground/effect working tiles | 24 |
| Gameplay sprite patterns | 28 |
| SAT source | 0.5 |
| UI/transition/upload reserve | 7.5 |
| Total | 64 |

This is a budget, not a measured fit. Report dirty-pattern upload bytes and pin on-screen patterns until the displayed SAT no longer references them. Double-buffer CPU SAT construction; account for VDC SAT DMA time and pattern changes together.

### 5.2 One background and selective sprite scenery

Use three complementary strategies:

1. Bake scenery that can share a scroll rate into one background, preserving the playfield and collision silhouette first. Quantize distant scroll rates to a few bands. Use scanline scroll splits where the art occupies different vertical regions.
2. Use background pattern animation, palette animation, and occasional CPU-updated composite tiles for inexpensive motion or small overlays. Split scrolling changes a whole horizontal region; it cannot produce two independent overlapping backgrounds on the same line.
3. Use the object-map technique from `PCE/Tilemap/Notes.txt` for **sparse** trees, props, rails, or foreground occluders. Compile visible metacells into clipped metasprites and put them before/after actors in SAT order as appropriate.

The sprite-object map should not be the universal second layer. Hardware has 64 SAT entries and a per-line limit of 16 **16-pixel sprite units**; 32-wide sprites cost two units. Transparent portions still consume evaluation capacity. Tall sprites can reduce total entries but do not reduce their width cost on intersected scanlines. The local Mednafen `pce_fast/vdc.cpp`, `RebuildSATCache` and `DrawSprites`, explicitly implement that accounting.

A continuous 256-dot sprite background already spends all 16 units per line. Even 208 dots needs at least 13 units for complete coverage, leaving only three for actors on that line. A 240-dot layer leaves almost none. The note's successful scene is sparse and deliberately clipped; its total sprite counter is not evidence that a dense Saber Rider foreground will fit.

Implement a per-scanline occupancy estimate in the asset tools and runtime. Cull transparent margins, use 16-wide pieces at horizontal edges, and trim vertical extent in baked descriptors. Prioritize the player, harmful projectiles, boss attacks, and essential occlusion; remove cosmetic scenery before essential sprites overflow. Any gameplay pool reduction must preserve encounter behavior or be documented as a PCE adaptation, not silently drop dangerous objects.

Sprite-to-background priority is binary; sprites among themselves follow SAT priority. A low-priority actor can disappear behind every opaque background pixel, including distant scenery. Recreate complicated layer order using foreground sprite pieces or selective tile compositing. Map `r_set_depth` to an explicit PCE composition plan rather than pretending the original depth slots are available.

### 5.3 Offline pipeline and API boundary

Build `game/tools/pce/` around existing pack extraction, the host graphics converters, and the Saturn layer extractor. Emit PCE planar characters, sprite patterns, palettes, metasprite descriptors, per-stage map columns, collision/triggers, animation metadata, scale/angle variants, and audio banks.

Use compact IDs and table offsets for `RTex`/sprite resources. `r_layer`/`r_layer_held` can own baked background layers; `r_floor_draw` dispatches to the PCE BAT sampler. Replace rendering-heavy portions of `mode7_draw`, `ramrod_draw`, and `space_draw` where the abstract texture API hides essential PCE decisions. Compile hero torso/legs combinations into full frames when that saves SAT units. Keep the original gameplay anchors, hurtboxes, and muzzle positions.

Replace arbitrary rotation with small baked angle sets. Replace broad alpha overlays with palette fades, masks, stippled art, or the constrained planar effect below. UI fonts and panels should be background characters where feasible. Reuse two-button controls with documented context/chords: II jump, I shoot, Run pause, Select plus an action for power; aim can use a held action plus direction. Racing can use Up accelerate/Down brake, I fire, II turbo. Offer direct mappings for a detected six-button controller and verify that power/aim chords cannot accidentally trigger ordinary actions.

## 6. Stage 2: Wolf3D BAT framebuffer for both phases

### 6.1 What the supplied trick provides

`PCE/Framebuffer/Wolf3D.txt` describes a palette-index framebuffer encoded in BAT entries. Prebuild 256 character patterns, one for every pair of 4-bit colors. Each tile's left four dots show one color and its right four dots show the other. A BAT entry's low character byte therefore encodes two logical pixels; the upper byte provides fixed character-address bits and a palette selection.

At a 512-dot display width, 64 entries give 128 logical pixels. Those dots fill the normal physical display width at the higher pixel clock; this is not a physically wider screen. A 64×128 logical BAT layout is a raster reinterpretation of the hardware's maximum 128×64 map, **not a native 64×128 VDC mode**. Raster scroll rewrites select the required half-row and tile row. With uniform pair-pattern rows, one logical sample row can be repeated over several display lines.

The pattern table occupies 256×32 = 8,192 bytes. A full 128×128 sample surface requires 64×128×2 = 16,384 bytes of BAT words, even though the packed color pairs alone contain only 8,192 bytes. Keep the upper-byte character-address bits intact: characters must sit beyond the BAT allocation, not accidentally point into BAT data. For example, an 8 KiB pattern table starting at VRAM byte `$4000` uses character indices `$200-$2FF`.

The VDC has no general BAT-base register. Allocating a second 16 KiB surface somewhere in VRAM does not make it displayable by a conventional page flip. Full-size buffering needs explicit raster/address design or timed update scheduling.

### 6.2 Practical floor-only layout

Begin with a **128×48 to 128×56 sampled floor**, expanded over roughly the bottom 112 display lines, and a tile-rendered sky/UI above it. Retain the two-pixel BAT encoding. The exact horizon is a PCE camera parameter, derived from the current 240-line presentation.

A feasible prototype layout uses the two 64-column halves of a native 128×64 BAT as two floor pages. With 56 sample rows, each page occupies 64×56×2 = 7,168 bytes; rows 0–55 hold the two pages and rows 56–63 leave 2,048 bytes for sky/UI BAT data. Update the hidden half, then change the raster table's horizontal half selection at VBlank. Each floor row has a physical stride of 128 BAT entries; writers must skip the other page, not assume a tightly packed surface.

The 14 tile rows of a 512×112 sky require 896 BAT entries. They can fit in the remaining 1,024 entries **if stored as 64-entry strips and raster-remapped across the halves**. A conventional 128-wide sky layout would need more physical rows. Account for scrolling guard columns and HUD entries; use a simpler sky or 48 floor rows if that headroom is insufficient. This is a proposed raster layout requiring a standalone proof, not an already working mode.

Allocate the full 16 KiB BAT area, the 8 KiB pair-pattern table, separate sky/UI characters, sprite patterns, and SAT source. A sample 64 KiB budget is 16 KiB BAT, 8 KiB pair patterns, 8 KiB sky/UI characters, 24 KiB sprites, 0.5 KiB SAT, and 7.5 KiB reserve. Dynamic sprite variants compete for this space.

### 6.3 Floor generation differs from Wolfenstein walls

Use **row-wise affine floor sampling**, not a wall-column raycaster. For a camera snapshot and each sampled floor row, obtain distance from a reciprocal table, compute starting world X/Y and X/Y increments, then walk across 128 samples. Resolve material IDs and quantized mip texels; combine two samples into one BAT character byte and choose a row/distance fog palette.

Bake the stage's material textures into one shared 16-color floor space. The source has 11 materials with separate RGBA colors; changing BAT subpalette affects both pixels in a pair, so it cannot provide arbitrary independent palettes for two different materials. Reserve a bounded number of background palettes for distance fog and the remaining palettes for sky/UI.

Cache nearby material-map chunks in mapped RAM and keep hot textures/tables there. Spatially random Arcade reads for both map and texel at every pixel are a likely bottleneck. Profile cache misses and use 16- or 32-pixel row spans when useful. The track data used by collision/rival movement and the visual floor map must remain consistent.

Write complete pixel pairs where possible. The note's odd-column `TSB $0002` optimization relies on VDC read-buffer/pointer behavior; it is not an ordinary framebuffer read-modify-write. If adopting it for another renderer, explicitly synchronize MARR/MAWR and verify prefetch/increment effects and IRQ interference. Pairwise floor generation can avoid it entirely.

Generated unrolled samplers may help, but keep executing code in mapped Super CD RAM. Arcade RAM is not executable. Bake only useful kernel variants; do not spend the mapped code budget on every scale/material combination. The note's estimates and its approximately 330-byte-per-line VRAM DMA claim are hypotheses for its wall renderer, not measured Saber Rider throughput. VRAM-to-VRAM DMA also requires actual source data and does not automatically supply a zero fill or whole-frame tear protection.

### 6.4 Sprites, timing, and both phases

At the 512-dot clock, a 16-dot sprite is approximately half the physical width it has in 256-dot mode. Preserve aspect with horizontally rebaked art or an intentionally different composition. Twofold horizontal expansion also doubles width-unit consumption; the 16-unit scanline ceiling remains. The source's 82-pixel-wide cars cannot all be shown at source size with unrestricted overlap.

Use smaller baked cars, several distance variants, and a cap on **visible overlapping** rivals. Keep all seven racers in simulation when possible; distant cars can use smaller sprites or be rasterized into the coarse floor. Put essential near mines/shots and the player's car in hardware sprites. Render far props into the BAT when their palette and cost permit; place tall scenery cautiously across the horizon. Do not depend on an emulator's unlimited-sprites option.

Race: retain laps, rival progress, turbo, road/off-road behavior, shots, mines, and finish placement. Pursuit: retain gap, escort, leader weaving/boost, catch transition, ramming, rear gunner, and destruction. Both use the same BAT floor engine with different map/track tables. Replace the briefing zoom with baked scale steps and palette white-out. Preload its assets with the stage.

Aim for 60 Hz input/collision and a 20–30 Hz floor refresh; use 15–20 Hz if profiling requires it. These are targets, not promised rates. Draw sprites against the **committed floor camera** so a newly projected car does not visibly slide against an older road. Capture a new camera when generating the hidden page and atomically commit it with the page. If game updates cannot meet 60 Hz, use an explicit measured simulation schedule and adjusted timing rather than an ever-growing catch-up loop.

## 7. Stage 6: coarse scales plus overlapping sprite slices

The supplied `PCE/scalingsprites/Scaling sprites - Plutiedev.html` combines pre-scaled coarse images with closer placement of their slices between sizes. It reduces the abrupt jumps of a pure size lookup. [Original scaling technique](https://plutiedev.com/scaling-sprites).

### 7.1 PCE implementation

1. Bake several coarse mech sizes per animation pose, palette variant, and needed facing. Store the complete set in Arcade RAM, with only current/next useful variants in VRAM.
2. Split each coarse image into PCE hardware-compatible pieces: minimum width 16 dots, heights 16/32/64. The reference's 8-pixel slice grid cannot be copied directly to native PCE sprites. Start with 16×16 pieces around silhouettes and use larger pieces for rigid areas when they reduce entries safely.
3. For a desired size, select a coarse image at or just above it and move slice origins toward the projected anchor. Precompute offset tables. Pieces retain their pixel data and overlap; they do not undergo true per-pixel scaling.
4. Fix overlap order, remove padded transparent pieces, and use small compression intervals. Prefer more coarse sizes when 16-dot pieces visibly deform faces, gun arms, or leg motion. Add hysteresis around size switches and retain a common foot/muzzle anchor across variants.
5. Generate SAT-entry and per-line width-unit costs for each size and pose. Overlap can increase scanline pressure even when the apparent sprite shrinks. Three mechs, cockpit arms, rocks, shots, and HUD cannot be budgeted separately.

The source mech frames are 138×146 (`game/assets/ramrod/atlas.txt`). A naive 16×16 grid would be 9×10 = 90 pieces before trimming, exceeding the whole SAT. Recompose/bake smaller PCE frames and use a mix of piece sizes. Never assume that the technique makes a source-sized nearest mech free. A temporary example width budget in 256-dot mode is an 80-dot nearest mech (five units), two 32-dot distant mechs (four units total), and seven units left for arms/shots/other intersecting art; actual poses and overlap must be measured.

### 7.2 Cockpit and ground composition

Prefer 256-dot display for the cockpit so slices and wide near mechs have more useful physical coverage. Bake cockpit regions into the BG; put panorama and a small floor window in other raster regions of that same BG where their screen areas permit it. Recompose a mostly rectangular viewing opening and clip high-priority mech pieces to it. Cull pieces fully outside; handle partial edge pieces with precomputed masks or a bounded working-pattern update. Use a few foreground cockpit/hand sprites for indispensable overlap and charge them to the same scanline budget.

Low-priority mechs cannot simply sit behind an opaque cockpit BG and in front of an opaque panorama/floor BG: binary priority would hide them behind both. A transparent BG opening can mask low-priority sprites, but its background must then be supplied separately. Prefer the explicit clipping plan with high-priority mechs over panorama/floor; prove its partial-piece upload cost in M2.

The current cockpit is 426×240, the panorama is 1,344 pixels wide, horizon is around row 167, and the game already calls `r_floor_draw` for a uniform-material sand floor. Recompose around a smaller PCE viewing window. The lower sand strip can use palette/character animation or a reduced BAT framebuffer derived from section 6; the scaling technique itself does not solve floor rendering. If using the 512-dot framebuffer for that strip, explicitly resolve dot-clock boundaries, sprite aspect, and width budgets; do not assume per-region clock switching is free or already proven.

Keep wave counts and the source's maximum of three simultaneously active wave mechs, reduce cosmetic debris/smoke/props, and retain punch range, lock targeting, heat, armor, radar, and attack telegraphs. Bake arm swing frames and prioritize a legible nearest threat over scenery. The source's 220-pixel-wide late arm frames also need PCE recomposition.

### 7.3 Space finale, internally stage 7

Use a separate 256-dot shooter layout: nebula/planet/far ship composed into the BG with scroll bands; a small star set through tile/palette animation; sprites for Ramrod, enemies, and dangerous projectiles. Convert particles and debris to bounded pools. Bake a few cruiser tilt states; align its gun ports and hit regions to those states.

Evaluate drawing the large cruiser as BG characters during its fight and expressing essential nebula/foreground elements separately. Use sprite sections only where motion or overlap needs them. Its hull should not consume all SAT entries and scanline units. Preserve the attack sequence and power bomb while adapting simultaneous cosmetic effects. The CPU-only bullet demonstration in `PCE/Other/bullets.txt` uses Lua for visual output; it is not evidence that hundreds of bullets can be rendered by the VDC.

## 8. Stage-by-stage background plan

| Stage | Background plan | Selective sprites/compositing | Main acceptance concern |
| --- | --- | --- | --- |
| 1: frontier town | Merge playfield/platforms/cars where their scroll rate matches; simplify distant mountains into scroll bands | Sparse foreground props, hero/enemies, convoy/boss | Hero feet and platform silhouettes; convoy/boss must remain readable under worst overlap |
| 2: Grand Prix and pursuit | Wolf3D BAT floor in both phases; raster sky/UI | Baked car distances, essential near mines/shots, selected props | No tearing; road/collision agreement; high-resolution sprite pressure |
| 3: Hyperjumper Pass | Reuse town assets with baked night palette and simplified mountain bands; BG moon/sky where possible | Hyperjumper flight/fight, sparse foreground | Boss passes and shots retain intended depth; palette changes do not recolor unrelated assets |
| 4: Red Palm Jungle | Collapse distant foliage rates; merge playfield/cabin art; use splits only across suitable vertical bands | Selected trunks/rails/foreground pieces | Preserve tower decks, shield snipers, cabin occlusion, and finale; original eight-rate parallax is reduced |
| 5: Cavern Laboratory | Merge MidBG + Playfield at rate 1; simpler distant cave bands | Gates/props and boss pieces, constrained energy effects | Both boss phases and Dark April behavior; no foreground hiding dangerous shots |
| 6: Power Stride | Rebuilt cockpit/window, panorama, small sand strip | Coarse sizes + slice compression for mechs; baked hands | Nearest mech/arms fit SAT; scale changes keep feet, muzzle, and hit feedback coherent |
| 6 finale: space | Nebula, stars, planet, and optionally cruiser in BG | Player, enemies, attacks, selected hull pieces | Collision objects remain visible; cruiser attacks survive particle reduction |
| Menus/dialogs/power | Tile artwork, fonts, portrait swaps, palette transitions | Few portrait/overlay sprites | All heroes and stage transitions work without video playback |

## 9. Additive/translucent effects from the supplied reference

`PCE/Transparency/Achieving_Transparency_effects.md` and `image1.png`, `image2.jpg`, `image3.png` were inspected. The images show the same four-color-per-tile base under several precomputed tints, with the final ghost mask selecting those tints.

Implement selected background effects with two lower base planes and two upper mask planes:

`index = base_color_0_to_3 | (effect_class_0_to_3 << 2)`

For each base subpalette, bake the 16 resulting colors, with effect class 0 preserving the base and classes 1–3 holding the desired additive/tinted results. Quantize the combined RGB values to VCE precision on the host. This yields three visible mask classes plus transparent/no-effect. An extra palette family can add another tint, as in the reference's mouth, at additional palette/BAT cost.

Use unique working tiles for affected screen regions so editing a mask does not change every reused instance of a character. Restore mask planes at the old location, write them at the new location, and track dirty tiles. Multiple independent masks cannot overlap freely in those same two planes. Apply a palette budget and a bounded dirty-tile upload budget.

Good candidates are small lab energy fields, background flashes, smoke/light against prepared terrain, and power effects during a frozen scene. This is palette-selected composition **inside background characters**. It does not make a hardware sprite translucent over arbitrary background or other sprites. Actors passing behind such an effect require deliberate tile compositing or a simpler effect.

The stage-2 16-color paired-pixel floor already spends all four bits on the floor index, so it cannot simultaneously use the same two-plus-two plane scheme. Use baked bright sprites, stippled masks, and fog/palette flashes there. A separate four-base-color floor with effect bits would be a distinct, lower-color renderer and should only be considered after visual comparison. The reference's roughly 26-scanline update figure is specific to that demo, not a budget guarantee for this game.

## 10. Audio: CD-DA, one hardware ADPCM voice, optional PSG samples

### 10.1 Music and sample routing

Convert the music table in `game/src/audio.c` and any added stage cues to 44.1 kHz stereo CD-DA tracks. Generate a logical-music-ID-to-track table; avoid hardcoded track indices spread through the game. Handle repeat/one-shot end points and boss transitions through BIOS CD-DA calls. Seamless software-style loop points and instantaneous seeks are not guaranteed by physical CD playback.

Preload an OKI-compatible CD ADPCM sample bank into its 64 KiB RAM. Route high-priority voices, major attacks, hurt/death, and distinctive long samples to that one channel. Short guns/impacts/alarms/engines use PSG wave/noise or selected DDA sample channels. Choose priorities and retrigger rules explicitly; ordinary gameplay must not repeatedly cancel a voice for every gunshot. At an illustrative 8 kHz and 4 bits/sample, 64 KiB holds about 16 seconds of samples before alignment; actual rates follow the hardware divider.

Use stage-specific sound banks and precompose overlapping cue/voice sequences when that improves the one-voice limit. Power attacks become still-art animations with ADPCM/PSG cues. Reimplement `music_set_volume` deliberately: CD-DA is not a software stream with arbitrary per-frame gain. BIOS offers particular fade modes; validate available control and use those fades or an adapted cue balance. Do not promise the existing smooth ducking envelope unchanged.

### 10.2 Six-channel DDA / 2-bit software ADPCM option

`PCE/Sound/adpcm_build_14_2bit.pce` is a 1 MiB binary; `pcm_fx_build_5.pce` is 256 KiB. No decoder source was identified alongside them. Treat the first as a reference to reverse engineer, not an existing linkable library. Its filename is not a proven sample rate or six-voice cycle budget.

Reverse-engineering tasks for an optional decoder:

1. Use the local HuC6280 disassembler and execution coverage to locate reset/banking, PSG writes, sample clock, and decoder entry points.
2. Recover the 2-bit packing, predictor/step tables, saturation, loop resets, mixing/output convention, and sample addressing. Check whether output uses DDA writes or another PSG technique rather than assuming it from the name.
3. Recreate a matching host encoder and small assembly player; compare decoded output and account for licensing/provenance before importing code.
4. Measure one, two, then additional independent sample channels with gameplay/raster active. Channels 0–5 are independently selectable PSG channels; they are not six hardware ADPCM decoders. Every software voice consumes CPU time and competes for sample delivery.

Start with one or two DDA voices plus PSG effects and the hardware ADPCM voice. The ordinary HuC6280 timer's finest tick is about 6.99 kHz at high CPU speed (1,024 clocks); higher rates need a different delivery mechanism. A binary demonstrating a faster rate does not establish that the same mechanism coexists with the game's scanline renderer.

### 10.3 Shared timing budget

At approximately 7.16 MHz there are roughly 120,000 CPU clocks per video frame. A HuC6280 block transfer has approximately `17 + 6*N` base clocks and defers interrupts until it ends, before accounting for I/O wait states. A 512-byte transfer alone takes about 0.43 ms: several scanlines and several maximum-rate timer ticks.

Consequently:

- Put sprite uploads mostly in VBlank; divide transfers according to audio latency limits even there.
- During an active BAT raster display, prefer interruptible paired writes or transfers sized to the available raster slack. A fixed 256/512-byte TIA chunk is incompatible with per-line deadlines.
- Preserve/restore the selected VDC register around IRQ work; protect low/high VWR commits with tiny critical sections. Keep raster handlers short and deterministic, with a documented nesting policy for audio.
- Leave the hardware ADPCM channel running during long loads; suspend software sample playback for blocking BIOS operations unless coexistence has been demonstrated.
- Keep audio sample readers and the Arcade upload reader on separate owned ports, or fully restore port state. Separate ports prevent address corruption but do not create extra CPU/bus bandwidth.

Measure floor sampling, entity logic, raster IRQ, SAT/pattern upload, and software audio **together**. If six software voices miss the budget, keep fewer sampled PSG voices and synthesize the remaining effects. Protect readable gameplay and stable audio timing first.

## 11. Numeric and simulation changes

Use the fixed-point game as the behavioral reference, then introduce a PCE numeric layer instead of changing `real` to a 16-bit type globally. Many world coordinates exceed the range of 8.8 fixed point.

Use 16-bit integer world positions plus an 8-bit fraction, or another explicitly bounded 24/32-bit representation, for long levels and the 8,192-unit racing world. Use 8.8/12.4-style arithmetic for bounded velocities, screen-local offsets, scale factors, and camera intermediates when their ranges permit it. Separate lap count from within-lap progress; do not keep accumulated race progress as a routine 64-bit value. Store timers as ticks where equivalent.

Bake trigonometric, reciprocal-depth, fog, scaling-offset, and per-frame velocity tables. Reduce track search to cached neighboring segments. Use packed structures of arrays for hot entities, compact state/animation IDs, and banked immutable definitions. Eliminate frequent 64-bit squared-distance tests with bounded/local comparisons where they preserve collision semantics.

The optional two-axis player-hit LUT in `PCE/Other/bullets.txt` may accelerate equal-size projectile checks. Evaluate it only for suitable attacks: different bullet radii, world collisions, and boss hit regions still need their intended rules. Do not use the Lua-rendered demonstration's counts as a target VDC sprite count.

## 12. Implementation sequence and completion gates

These are future implementation/validation tasks. This planning change adds no tests and runs no game benchmarks.

| Milestone | Deliverable | Gate before proceeding |
| --- | --- | --- |
| M0: asset/capacity audit | Per-stage inventories; layer/depth plan; palettes; worst-case sprite occupancy; exact toolchain pin | Identify oversized state, visible effects, both stage-2 working sets, and nearest stage-6 poses |
| M1: LLVM-MOS disc + Arcade runtime | Bootable IPL/application, BIOS integration, all-port driver, BRAM save structure, overlay transitions | Boot with correct hardware; missing-card/old-BIOS message; mapped bank, stack, port, and transfer checks |
| M2: render feasibility prototypes | Wolf BAT floor with sky + moving sprites; largest sliced mech with cockpit + shots; one constrained planar effect | Tearing, raster deadlines, palette, SAT, aspect, and audio coexistence demonstrated |
| M3: offline asset builder + stage 1 | Native graphics/audio packs; player/enemy core; town, dialogs, boss, HUD | Stage completes with collision/depth preserved and no data reads while CD-DA plays |
| M4: complete stage 2 | Racing, finish/briefing, pursuit, boss, continue-from-phase-2 | Both phases share the Wolf technique; map/render alignment, RAM-only transition, stable measured refresh |
| M5: stages 3–5 | Night pass, jungle/cabins, cave/lab and both bosses | All encounter/occlusion/power cases work within sprite and palette limits |
| M6: stage 6 + space finale | Slice scaling and cockpit combat; cruiser shooter overlay | Nearest mechs/arms and densest essential shooter attacks remain visible and timed |
| M7: complete presentation | Four heroes, menus/options, still-art replacements, CD-DA/SFX, saves/continues | Full run and stage transitions; physical-disc timing; no persistent bank/VRAM/sample leaks |

Prove the difficult renderers at M2 before committing to the whole port. If a renderer misses its budget, first reduce floor rows/refresh, cosmetic density, scale intervals, and preserved parallax; retain both requested stage-2 phases and the requested stage-6 scale method. A toolchain switch alone will not remove VDC limits.

## 13. Validation resources and unresolved risks

Use `PCE/mednafenPceDev-main/mednafen/src/` for hardware behavior inspection:

- `hw_misc/arcade_card/arcade_card.cpp/.h`: four ports, sequential increment, mirrored banks, physical RAM masking.
- `hw_video/huc6270/vdc.cpp`: BAT addressing, BXR/BYR latching, VRAM read/write buffers, DMA arbitration. Compare `pce_fast/vdc.cpp` for the explicit sprite-unit accounting, but use the accurate core for timing-sensitive conclusions.
- `pce/huc6280.cpp` and `huc6280_ops.inc`: HuC6280 opcodes, transfer and timer behavior. The HuC6280 is not identical to a generic 65C02, including some status-flag details.
- `hw_sound/pce_psg/pce_psg.cpp` and `pce/pcecd.cpp`: DDA behavior, six channels, CD/ADPCM integration.
- `PCE/mednafenPceDev-main/README_HEADLESS.md`: existing headless screenshot, memory, disassembly, coverage, and persistent-control tools. Trace bank changes as well as logical PCs; logical coverage alone cannot distinguish different code banks at the same address.

Do not treat emulator behavior as final proof of raster timing. Disable unlimited sprites and confirm Arcade Card emulation. The explicitly visible firmware under `PCE/` is a v2.0 System Card; Super CD/Arcade validation needs suitable v3.x firmware and the corresponding enabled expansion. Confirm available firmware before reporting a successful boot.

When implementing, collect deterministic captures for platform bosses, jungle tower decks, Dark April, race start/corners/finish, pursuit mines/ramming, the nearest Commander, and the cruiser's densest attacks. Record sprite units per scanline, missing essential sprites, VRAM upload bytes, raster lateness, DDA jitter, active-state high water, and disc reads. Keep a host preview of quantized assets and scale intervals.

Finish with real PC Engine/Arcade Card testing for 512-dot timing, BYR effects, palette composition, CD-DA seeks, sample delivery, and disc retries. The experimental secret-register/interleaved-display discussion in `PCE/Framebuffer/VDCsecrets.txt` and `Sprites.txt` is not a dependency of this initial plan.

The largest remaining risks are floor sampler throughput with map caching, high-resolution sprite coverage in stage 2, nearest-mech/arm overlap in stage 6, and essential projectile visibility in the space finale. The compiler choice is comparatively well supported: **LLVM-MOS plus the existing CD target and a bounded Arcade driver is the recommended path; HuC/HuCC is the reference/fallback for a deliberate PCE rewrite, not a prerequisite for Arcade CD support.**
