# Sega Saturn port — plan

Target: a **stock Sega Saturn** (no RAM cartridge, at least at first), NTSC 60 Hz, a CD image with the data track
plus CD-DA music. The port reuses the shared core (`src/*.c`) through `src/platform/`, the same way the Dreamcast
port does, and adds a `src/platform/saturn/` backend, a `Makefile.saturn` and a `tools/saturn/` disc builder.

References (all under `../Saturn/` unless noted):

| What | Where | Used for |
|---|---|---|
| libyaul toolchain + build scripts | `~/Documents/DEV/libyaul-build-scripts/`, installed at `~/.local/x-tools/sh2eb-elf` (GCC 13.2, `build.pre.mk` / `build.post.iso-cue.mk`, `bcl_*`, `make-iso`, `make-cue`, `satconv`) | the build |
| libyaul examples | `libyaul-examples/` (`vdp2-rbg0`, `vdp2-all-nbgs`, `vdp2-line-scroll`, `vdp2-24bpp-bitmap`, `vdp2-special-function`, `vdp1-zoom-sprite`, `vdp1-mesh`, `dma-queue`, `cpu-dual`, `cd-block`, `scsp-ponesound-pcm8`, `scu-dsp`, `gamemath`) | how each piece of hardware is driven |
| a small Saturn game | `VN_game_saturn_wip/` (NBG0 bitmap, BCL/PRS decompression, CD file reads, CD-DA `cdplay`, cue with audio tracks `game-cdda.cue`, adp68k ADPCM driver) | CD-DA, CD file access, the cue layout |
| Cinepak player | `libyaul_cinepak/` (Sega FILM `.CPK`, 24bpp decode into a 512x256x32 NBG0 bitmap, PCM/ADX audio through pcmsys + the SCSP DSP) | FMV |
| test emulator | `mednafen-headless-debug-kit/` (stdin/stdout protocol + MCP bridge: frame stepping, screenshots, WAV capture, SH-2 memory/regs, exact per-instruction coverage, save states) | automated tests, profiling |

---

## 1. The machine and what it means for this game

| Resource | Stock Saturn | Dreamcast (for scale) | Consequence |
|---|---|---|---|
| CPU | 2x SH-2 @ 28.6 MHz, **no FPU**, 4 KB cache each, DIVU unit | SH-4 @ 200 MHz with FPU | float code must go (section 8); the slave SH-2 has to carry real work |
| Work RAM | 1 MB HWRAM (fast) + 1 MB LWRAM (slower, **not reachable by SCU DMA**) | 16 MB | no pack stays in RAM; everything is baked and compressed; LWRAM holds compressed data the CPUs read, never DMA sources (8.5) |
| VDP1 | 512 KB VRAM (commands, gouraud tables, sprite pixels) + 2x 256 KB framebuffers | 8 MB VRAM shared | sprites 4bpp wherever the loss is low, LZ4 in RAM, a sprite cache with eviction |
| VDP2 | 512 KB VRAM in 4 banks of 128 KB, 4 KB CRAM (2048 colours in mode 1), NBG0-3 + RBG0 | — | tile layers, Mode 7 and bitmaps; the layer count and VRAM access-cycle budget are the limit |
| Sound | 68EC000 + SCSP, 512 KB sound RAM, 32 slots, DSP | AICA, 2 MB | CD-DA for music, a new sfx driver (section 7) |
| CD | 2x (~300 KB/s), 512 KB CD block buffer | GD-ROM | nothing is read from disc during play; CD-DA and data reads exclude each other |

The core is 15k lines of C11 with **~900 lines using float** (`mode7.c` 186, `ramrod.c` 156, `space.c` 152,
`enemies.c` 102, `menu.c` 71 …) and ~290 libm calls (`floorf` 125, `sinf` 70, `cosf` 35, `hypotf` 24, `atan2f` 18).
`sizeof(Game)` is **383 KB** on a 32-bit target (`Enemies` alone 215 KB), and stage 2's `Mode7` struct holds a
**1 MB** material map. These three numbers drive most of the core-side work.

---

## 2. Decisions (summary)

| Topic | Decision | Section |
|---|---|---|
| Resolution | **320x224** (4:3, `VDP2_TVMD_HORZ_NORMAL_A`) and **352x224** (wide, `NORMAL_B`), `VERT_224`, non-interlaced; game logical height becomes 224 | 3 |
| Tile layers | **VDP2 NBG0-3**, cell mode, 8x8 cells with a palette per cell, streamed from RAM | 4.1-4.3 |
| Mode 7 | **VDP2 RBG0** with a per-line coefficient table; the floor map is a sliding window | 5 |
| Sprites, HUD, text, effects | **VDP1**, palette (colour bank) sprites, per-sprite priority bits to sit between VDP2 layers | 4.4 |
| Pixel depth | **4bpp (16-colour banks) by default**, 8bpp only where the measured loss is too high; RGB only for FMV | 4.5 |
| Compression | **LZ4** for everything stored in RAM or on disc (decoder already in the tree: `dcfmv/lz4_mini.h`) | 9 |
| FMV | libyaul_cinepak, Sega FILM, **24bpp** | 6 |
| Briefing | whole screen is the 24bpp bitmap (room baked in, video decoded into its rectangle), text + hero pieces on VDP1; fallback = video + text only | 6.2 |
| Music | **CD-DA** tracks (22.3 min for all 18 tracks, ~235 MB) | 7.1 |
| SFX / voices | one 68k driver shared by the game and FMV, no 64 KB sample limit | 7.2 |
| Math | fixed point (16.16) in the shared core for the hot paths, LUT trig, DIVU for division | 8 |
| Second CPU | slave SH-2: VDP1 command lists, tile streaming, LZ4, Cinepak, Mode-7 coefficients | 8.3 |
| Tests | mednafen headless kit + a small patch for pad input + a log ring read over the debugger | 11 |

---

## 3. Video mode and the 240 → 224 change

- `VDP2_TVMD_VERT_224` gives 224 lines (240 lines exists on NTSC, but 224 is the safe area on real TVs, and it is
  what was asked for). The VDP1 framebuffer is 16bpp, 512x256, and both 320 and 352 fit in it.
- **320 ↔ 352 needs an SMPC clock change** (`CKCHG320` / `CKCHG352`). That command stops the slave SH-2 and the
  sound CPU, so switching RATIO means: black screen → clock change → restart the slave → reload the sound driver
  and sound RAM → re-init VDP1/VDP2 → reload the stage's VRAM. **This needs verifying in mednafen.** If it holds, RATIO is
  applied at boot (a boot-time choice, saved to backup RAM) or through a controlled re-init from OPTIONS, the
  way the Dreamcast's 832x480 switch re-inits the PVR.
- The core already takes `sw`/`sh` from the platform (`game.c:24`, `plat_wide_width`). What needs auditing for 224:
  - camera vertical framing on every platform stage (level 1 is 256 px tall; with 16 fewer rows, decide per stage
    whether to follow the hero vertically or trim the sky);
  - HUD, dialog box geometry (`dialog.c`, FUN_0042a5e0 / FUN_00475610), title card band, CONTINUE screen, the
    power-attack cut-in, Mode 7's horizon/`y0..y1`, the ramrod cockpit (`ramrod.c` `ART_W 426`, row 167 = sand);
  - full-screen art made for 426x240 / 320x240 (victory paintings, menus, cockpit, briefing room): re-cut at
    build time into 352x224 and 320x224 versions (crop + scale chosen per image, listed in a table the builder reads);
  - `grep -n 240 src/*.c` gives 18 hits in 9 files to review.
- `plat_screen_modes` on Saturn: none (no SCREEN option), `plat_wide_width` = 352, `plat_screen_43_only` = false.
- PAL (50 Hz) is left out at first; the fixed-step accumulator already copes with 50 Hz frames if it is wanted later.

---

## 4. Graphics architecture

### 4.1 Why the render API needs two additions

`render.h` is immediate mode: every frame the core draws each tile layer as hundreds of 16 px quads
(`r_tex_batch`). That suits the PVR, but on the Saturn those layers must be **retained VDP2 scroll planes**. Drawing
them with VDP1 would be ~1000+ tiles and several screens of overdraw per frame, which VDP1 can't fill at 60 fps.
The header already anticipates this ("draws the floor with its hardware rotation plane"). The same pattern is
applied to tile layers and draw order:

1. **Scroll planes** (new, like `r_floor_*`):
   `r_plane_create(Ren*, const RPlaneDesc*)` (a tile map + its tile bank + wrap flags),
   `r_plane_scroll(p, x, y)`, `r_plane_line_scroll(p, const int16_t *per_line_x)`, `r_plane_tint(p, r, g, b)`,
   `r_plane_destroy`. `level_draw_layer` calls these when the backend reports `r_has_planes()`. A
   `platform/common/plane_soft.c` gives the SDL3 and Dreamcast builds the old behaviour through `r_tex_batch`, so
   they don't change.
2. **Draw layer / depth** (new): `r_set_depth(int layer)`, called by the core where it already interleaves sprite
   layers between tile layers (the `PlayerSprites`, `Small Hyperjmpr`, `MidBGHyperjpr` layers, forest
   `ForegroundStuff` drawn over cabin decks, `enemies_draw_front`). The Saturn backend turns it into the VDP1
   sprite's priority bits, which pick one of VDP2's 8 sprite priority registers. That puts a sprite between two
   NBGs. PC and Dreamcast ignore it (painter's order already holds).
3. **Baked textures**: every `rtex_create*` from pack/PNG data goes through `rtex_create_baked` with a Saturn
   block (`SAT1`, see 4.5), as the Dreamcast does with `PVT1`. `rtex_create` from runtime RGBA is kept only for
   small generated textures (e.g. the scanline mask, which is dropped on Saturn) and is otherwise an error on this
   backend.
4. Optional fast paths the backend may expose: `r_fade(r, g, b, level)` (screen fades as VDP2 colour offset
   instead of a full-screen translucent rectangle), `r_backdrop(tex)` (full-screen pictures as a VDP2 bitmap).

### 4.2 VDP2 layout for the platform stages (1, 3, 4, 5)

Level 1 has **11 tile layers** (measured from `levels.pck` 12DAD1A7). The parallax values:

| Layer | Parallax | Tile bank | Map (cells) |
|---|---|---|---|
| SkyBG | 0 (wraps) | B13D9860 | 61x11 |
| FarMountains | 0.2 | B13D9860 | 440x17 |
| Mountains | 0.3 | B13D9860 | 440x33 |
| NearMountains | 0.4 | B13D9860 | 440x30 |
| MidBG | 0.875 | 7146108C | 656x31 |
| Cars MidBG | 0.9 | 13619DBE | 509x21 |
| Playfield | 1.0 | D8B018EE | 644x67 |
| Platforms | 1.0 | D8B018EE | 566x16 |
| Cars | 1.0 | 13619DBE | 593x22 |
| ForegroundStuff | 1.0 | D8B018EE | 531x18 |
| ForegroundStuf2 | 1.2 | EAB0BEF6 | 782x18 |

VDP2 has 4 normal planes. The stage builder (`tools/saturn/layers.py`) collapses layers per stage. It works from
three tools, and reports what each merge costs:

- **Merge equal-parallax layers into one plane.** Their overlaps are baked into composite cells (new unique
  cells). Playfield + Platforms + Cars go on one plane at 1.0.
- **Per-cell priority bit.** In 2-word pattern names each cell carries a special-priority bit. Cells of
  ForegroundStuff that must cover sprites can live in the playfield plane and be drawn over VDP1 sprites. Where
  a foreground cell overlaps playfield art in the same 8x8 cell, it stays on its own plane.
- **Line scroll bands.** Layers that are vertically disjoint on screen share one plane with a per-line horizontal
  scroll table (sky rows at 0, far mountain rows at 0.2, …). Where far layers overlap, the builder bakes them into
  one layer. Each scanline then scrolls at the parallax of the front-most opaque layer on that row
  ("dominant-layer line scroll"), the classic 16-bit port trick. The builder renders the result against the PC
  renderer over a camera sweep and reports the pixel error so it can be judged.

Proposed level-1 assignment (to be confirmed by the builder's report):

| Plane | Contents | Notes |
|---|---|---|
| NBG0 | ForegroundStuff (+ ForegroundStuf2 at 1.2 if it merges acceptably, else VDP1) | above sprites |
| NBG1 | Playfield + Platforms + Cars (1.0) | behind sprites, special-priority cells for fronts |
| NBG2 | MidBG (0.875) + Cars MidBG (0.9) | merged at one factor, or line bands |
| NBG3 | SkyBG + Far/Mid/NearMountains | dominant-layer line scroll |
| Back screen | sky gradient colour per line if the sky allows it | frees NBG3 rows |

The night tint of stage 3 (`night_layer_tint`) maps to **VDP2 colour offset A/B**, which is set per plane and
per sprite layer. It costs nothing, and the tinted copies of the palettes aren't needed.

**VRAM access cycles:** a 4bpp NBG needs one pattern-name and one character read slot. Four 4bpp NBGs fit in the
normal-resolution cycle table with room to spare; an 8bpp plane costs two character slots. The layout follows the
libyaul `vdp2-all-nbgs` example and the VDP2 manual's slot restrictions. Every layout is checked in mednafen,
because a wrong cycle pattern shows as garbage only on some emulators and on hardware.

### 4.3 Tile streaming (VDP2 character cache)

The playfield bank alone is 2020 tiles of 16x16 (**252 KB at 4bpp**, 505 KB at 8bpp); level 1's banks total about
**2.6 MB at 8bpp**. That doesn't fit in VRAM or RAM uncompressed. So:

- Tiles are split into **8x8 VDP2 cells**. Cells are deduplicated, including flipped copies, since the pattern name
  has H/V flip bits.
- Each plane has a **character cache** in VRAM (e.g. 2048 cells = 64 KB at 4bpp per plane) and a **pattern-name
  ring**: one 64x64-cell page (512x512 px, 2-word names = 16 KB) scrolled toroidally. As the camera moves, the
  slave SH-2 decodes the next columns (LZ4, in column chunks of 32 cells), looks their cells up in the cache
  (LRU by last-visible frame), uploads the missing ones and writes the new pattern-name column. This happens
  during the frame. It decodes into a small HWRAM staging buffer, and the VRAM writes go from there through the SCU
  DMA queue in vblank.
- The column chunks live compressed in LWRAM, loaded at stage start. The slave's LZ4 decoder reads them with the
  CPU; SCU DMA never touches LWRAM. No CD access happens during play (section 9).
- Budget per platform stage: 4 planes x (64 KB cells + 16 KB names) ≈ 320 KB, plus line-scroll tables (~4 KB)
  and back-screen/line-colour tables. That leaves ~180 KB of VDP2 VRAM for the stage's static extras and headroom.

### 4.4 VDP1: sprites, HUD, text, effects

- Every sprite (heroes, enemies, bosses, bullets, effects, HUD, fonts, dialog boxes, Mode-7 billboards, mechs) is a
  VDP1 command. Normal sprites draw unscaled, scaled sprites cover zoom, distorted sprites cover rotation
  (`r_tex_rot`), polygons cover `r_geometry`/`r_fill_rect`, polylines cover `r_rect`/`r_line`. Clipping
  (`r_set_clip`, viewport) uses user clipping plus the system clip.
- The command list is built on the slave SH-2 into a double buffer in HWRAM and SCU-DMA'd to VDP1 VRAM each
  frame. Target:
  ≤ 600 commands a frame (level 1's worst scenes need measuring on PC with `SABER_PERF` first).
- The characters are drawn by the core as cblock cells (`cblock_draw_frame`). The builder merges adjacent cells
  that share a palette into larger VDP1 sprites (widths multiple of 8), which keeps the command count low
  without losing the per-cell palettes (4.5).
- **Sprite pixel cache** in VDP1 VRAM (~400 KB after commands and gouraud tables). Sheets stay LZ4-compressed in
  LWRAM, frames are decompressed on first use (CPU reads LWRAM, writes an HWRAM staging buffer, SCU DMA into VDP1
  VRAM, or the CPU writes VRAM directly for small frames) and evicted LRU. This is the Dreamcast's evictable
  texture cache (`r_set_evict_hook`, `gfx.c` `last_used`) at frame granularity, not sheet granularity.
- VDP1 has no general alpha, so each effect gets a mapping:

| Core effect | Saturn |
|---|---|
| alpha fade of the whole screen, white-outs, wipes | VDP2 colour offset (fade to black/white), VDP2 window for the venetian-blind title-card wipe |
| translucent box (dialog backgrounds) | VDP1 half-transparency (50%), or VDP2 colour calculation on the sprite layer |
| `R_BLEND_ADD` (power attacks, space, ramrod, Dark April glow) | VDP2 colour calculation in add mode for sprites whose colour-calc bits are set (per palette sprite) |
| per-sprite alpha ramps (motes, vapour death, afterimages) | 50% half-transparency, VDP1 mesh (dither) for the other steps; accept stepped fades |
| colour mod: hit flash, Dark April violet, night tint on sprites | alternate CRAM palettes (flash = one all-white palette), colour offset for whole layers |
| `r_fill_rect` with alpha (fades, scanlines) | colour offset / half-transparent polygon; the SCANLINES option is hidden on Saturn |

### 4.5 Pixel formats: 4bpp by default (measured)

VDP1 colour-bank sprites and VDP2 cells both support 16-colour (4bpp) and 256-colour (8bpp) banks out of the same
2048-entry CRAM (128 palettes of 16). Index 0 is transparent, so a 4bpp palette has **15 usable colours**.

Measured on the demo's 21 tile banks / cblocks (5176 tiles, ARGB1555 source colours):

| Split | Tiles exactly representable at 4bpp |
|---|---|
| one 15-colour palette per 16x16 tile | 86.2 % |
| one palette per **8x8 cell** | **96.2 %** |

Per bank: the terrain banks are 95-100 % exact per 16x16 tile (B13D9860 100 %, D8B018EE 95.2 %, 7146108C 94.8 %,
EAB0BEF6 100 %), and 99.6-100 % with 8x8 cells. The 64x64-tile banks (large boss / scenery pieces, e.g.
1F909785 3.2 % per tile) rise to 64-92 % with 8x8 cells. The hero-piece art 2DEF1664 is the worst (44 %).
A **whole-frame** 15-colour palette for characters is too lossy: 23-32 dB PSNR, visible. So characters use
per-cell or per-part palettes, never one palette per frame.

Policy the baker applies per cell (VDP2) or per sprite part (VDP1):

1. ≤ 15 colours → 4bpp, exact.
2. More → 4bpp with a local 15-colour quantisation if the error is under a threshold (max ΔE in Lab and PSNR per
   part; start at ≥ 40 dB and no ΔE over ~6 on opaque pixels). Tune it by looking at contact sheets the tool
   writes next to the PC originals.
3. Otherwise 8bpp (a 256-colour bank shared by the stage).
4. **Palette packing:** the per-cell palettes of a stage are merged greedily (union ≤ 15 colours) into the CRAM
   budget. Per stage: ~64 palettes for VDP2 planes, ~48 for sprites (heroes and HUD resident), the rest for
   fonts/effects/flash palettes. The builder fails loudly with the list of offenders when a stage doesn't fit.
5. Everything the baker writes is LZ4-compressed. The report per stage lists 4bpp/8bpp counts, the loss stats,
   CRAM use, VRAM use and compressed RAM size (the Dreamcast's `SABER_VRAMLOG`, done offline).

RGB (15-bit) is used only for the FMV bitmap (24bpp) and, if a picture needs it, a one-off full-screen image on a
bitmap plane.

### 4.6 Other screens

- **Menus / title / splashes / character select / credits:** full-screen art as a VDP2 bitmap (8bpp palette for
  most, 16bpp RGB where a picture's quantisation fails the threshold). 352x224 at 16bpp in a 512x256 bitmap is
  256 KB, which is fine because nothing else is on VDP2 then. Zooming splashes ("sprite zooms in from 32x") use
  a VDP1 scaled sprite, or NBG0/NBG1 zoom if the source is a bitmap plane. The rotating spiral of the
  character select can use RBG0 or line/vertical cell scroll; pick whichever is cheaper after trying it.
- **Stage 6 phase 1 (cockpit):** RBG0 floor (section 5), the 360° panorama as a wrapping NBG with horizontal
  scroll, the cockpit overlay as a top-priority NBG (static, 4bpp cells), mechs/plasma/radar as VDP1.
- **Stage 6 final phase (space shmup):** starfield/nebula NBGs with line scroll, the cruiser boss as NBG
  (it is big) or VDP1 parts, bullets/ships VDP1. Bullet count is the risk: measure the PC build's worst frame.

---

## 5. Mode 7 on RBG0 (stage 2, stage 6)

`r_floor_draw` becomes an RBG0 setup:

- **Rotation parameters A** with a **per-line coefficient table** (kx/ky per line = the perspective scale),
  computed each frame from `RFloorView` (`cam_h`, `focal`, `horizon`, `y0..y1`) on the slave SH-2 in fixed point.
  The table is ~224 x 4 bytes. It lives in a VRAM bank designated for coefficients, or in CRAM (which halves
  the palette space, so VRAM is preferred).
- **Map = sliding window.** The floor is a 1024x1024 map of 8-unit cells, and **one 8-unit floor cell = one 8x8 VDP2
  cell**. Material *m* is 32x32 texels, so it becomes 16 cells indexed by `(cx & 3, cy & 3)`. The whole
  map (8192 units) is larger than RBG0's largest character map, so RBG0 shows a **256x256-cell window** (4x4
  planes of 64x64, 1-word names, 128 KB = one bank) with screen-over set to repeat. The window is re-centred
  by rows/columns as the camera moves, the same idea as `floor_pvr.c keep_window` (shift by whole periods,
  no full rebuild). The fog distance keeps the visible floor inside ±1024 units; the builder checks this
  against `fog1`.
- **Bank use during Mode-7 stages:** A0 = RBG0 characters (materials, 8bpp if the materials need it: ~16 KB),
  A1 = RBG0 pattern names, B0 = coefficient table, B1 = NBG0 (sky / mountain strip via line scroll, the level-1
  layers as now) + NBG1 (HUD pieces that are cheaper as cells). Billboards, cars, shots, the buggy: VDP1 scaled
  sprites.
- **Fog:** a per-line line-colour screen blended by colour calculation on the far rows, or the back screen
  showing through beyond `fog1`. Try it in mednafen and compare against the PC frames. No mipmaps on RBG0:
  far-distance shimmer is handled with rotation parameter B (mode 2: A/B chosen per line from the coefficient
  table), whose map plane points at a second name table using pre-blurred material cells for the far rows.
  That second table is optional and costs another 128 KB, so it only goes in if the shimmer is bad.
- **The 1 MB material map in RAM goes away.** The sand is `rng`-random per cell (`mode7.c:216`) → replace with a
  hash of (cx, cy) computed on the fly. The track/kerb/rock cells are stored as per-row RLE runs (the track
  is a band), a few tens of KB. `cell_at` (physics, off-road, solids) reads the runs. The window update uses
  the same function. This is a core change (all platforms), kept byte-identical by comparing `SABER_M7MAP`
  dumps before and after.

---

## 6. FMV (libyaul_cinepak, 24bpp)

### 6.1 Player integration

- `video_open` / `video_open_file` / `video_update` / `video_draw*` are implemented over `film_lib`
  (`cpk_task`, `copyVideoFrame`). The player decodes into a RAM buffer (`decode_buffer`, up to 320x240x4 = 300 KB)
  and `copyVideoFrame` moves it with **SCU DMA**, so as shipped that buffer must be in HWRAM. Options, measured in
  this order: (a) keep it in HWRAM, sized to the actual clip (briefing 256x104 = 104 KB, intro 352x176 = 242 KB,
  power clips 300 KB, which is where HWRAM gets tight mid-level, see 8.4); (b) put it in LWRAM and replace
  `DMA_ScuMemCopy` with the **SH-2 DMAC** (it can read LWRAM and write VDP2 VRAM; channel 1, since pcmsys may use
  channel 0 for `PCM_XFER_SH2_DMA`), checking throughput (~7 MB/s needed at 320x240x4 x 24 fps); (c) have the
  Cinepak decoder write its 32-bit pixels straight into the VRAM bitmap and drop the copy. It copies into an **NBG0 RGB 16M-colour bitmap, 512x256 x 32 bit = all 512 KB of VDP2 VRAM**.
  So while a video is on screen, **no other VDP2 layer exists**. Anything else on screen is VDP1, which has its
  own VRAM and framebuffer and is composited over NBG0.
- Encode: `ffmpeg -c:v cinepak … -f film_cpk` (both present in the local ffmpeg), audio 16-bit PCM or ADX (the
  player supports FILM sound codec 2 = ADX through the SCSP DSP). A `tools/saturn/film.py` step replaces the
  Dreamcast's DCMV conversion. Check the player against ffmpeg-made files first (header fields, strip counts)
  with the unchanged `libyaul_cinepak` main in mednafen.
- Videos: the intro `E46721E5` (768x384, 60 s → 352x176 / 320x160 letterboxed), the briefing `2FE798C3` (768x312,
  6 s, shown at 1/3 = 256x104), the power clips `assets/power/*.m4v` (320x240, 24 fps, 11-13 s, sound in separate
  WAVs → muxed into the FILM). `3EC59DE4` (heroes) stays unused, as on PC.

### 6.2 The briefing screen (24bpp)

The PC briefing draws the room picture (`0EAE8AEB`), the video at 1/3 scale on the room's big screen (unfolding
from its centre line), the green dialog text, then the four hero pieces sliding in.

- **Plan A (preferred):** the whole screen is the 24bpp NBG0 bitmap. The room picture is written into it once, at
  full colour, and each video frame is copied into its 256x104 rectangle. The unfold animation copies room rows
  instead of video rows outside the current half-height. Dialog box, text and hero pieces are VDP1 palette
  sprites above NBG0. VRAM is full, but nothing else needs VDP2 there. The only extra cost is copying the room
  once (~300 KB, decoded from LZ4 by the CPU into VRAM in slices; the source isn't SCU-DMA-able if it sits in LWRAM).
- **Plan B (fallback, as suggested):** only the video on screen (centred, larger, e.g. 2/3 scale), with the text
  below it on VDP1. Use it if Plan A's copies miss the frame budget.
- **Music during the briefing:** on PC and Dreamcast the menu music keeps playing under the voice-only video. On
  Saturn the music is CD-DA, and the drive can't stream the FILM at the same time. So the briefing FILM
  (256x104, 6 s: a few hundred KB of Cinepak) is **preloaded into LWRAM** and played from RAM while CD-DA
  continues. That needs a RAM source in `film_buff.c` instead of the CD sector reader. The decoder reads the
  compressed stream from LWRAM with the CPU, which is fine; only the decoded frame has to be DMA-able (HWRAM,
  104 KB here). The FILM's audio chunks go to sound RAM with the SH-2 DMAC or CPU copies, not SCU DMA.

### 6.3 Power clips mid-level

These are 11-13 s at 320x240, far too big to preload. The world is already frozen under the cut-in, so: record the
CD-DA position (current FAD from the CD block status), stop CD-DA, stream the FILM, then resume the level track
at the saved FAD. The clip's sound replaces the music while it plays, as on PC. The sprite state, sound RAM
and VDP1 cache stay put. VDP2 VRAM is lost to the 24bpp bitmap, so the stage's planes are re-uploaded after the
clip from the LZ4 copies in LWRAM (≈ 320 KB of decompression). Measure that; it runs under the clip's fade-out.
If it's too slow, the clip plays as 15bpp in half of VRAM and only the stage's other half is kept.

---

## 7. Audio

### 7.1 Music: CD-DA

- 18 tracks, **1336 s** total (measured from the converted OGGs) → Red Book audio tracks 2..19 on the disc, cue
  layout as in `VN_game_saturn_wip/game-cdda.cue`. The CD block plays them with a repeat count (the `cdplay`
  there shows the command). CD-DA costs no CPU and no RAM.
- `aud_music_play(id, loop)` maps a MUPS id to a track number (table made by the disc builder).
  `aud_music_pause` = pause/resume via the CD block; `aud_music_gain` = the SCSP external-input level.
- **Fades and ducking:** the EXTS send level (`EFSDL`) has only 8 steps, too coarse for the core's fades. The
  CD input goes through a tiny SCSP DSP program with a coefficient we write per frame (libyaul_cinepak's
  `scsp_dsp.c` shows how to load one), which gives smooth fades and the dialog duck.
- **Rules that follow from CD-DA:** no data reads while music plays. The Dreamcast design already loads every
  stage's assets at stage start, so the rule is to load, then start the track. A music change (boss, jingle)
  is a seek, with up to ~0.5-1 s of silence on hardware; the core's fade-out covers most of it.

### 7.2 SFX and voices: a new 68k driver

The VN game's `adp68k` driver (celeriyacon, shipped here only as a binary `adp68k.bin.inc`) decodes ADPCM on the
68000, but it caps a sample at **64 KB** and has 8 channels. The SCSP's own looping is also limited: a slot's
loop start/end registers are 16-bit sample counts (≤ 65535 samples per hardware loop), so any design that plays
a sample by pointing one slot at it hits the same cap. The fix is to never let a slot play the sample directly:

- **Short one-shots (most of the demo's 33 pack sfx, ≤ 4.7 s):** raw 8-bit PCM (or 16-bit where 8-bit hisses
  audibly) in sound RAM, played by an SCSP slot directly. Zero decode cost, up to ~24 slots.
- **Anything longer than a hardware loop, or compressed:** the 68k decodes 4-bit ADPCM (or ADX) into a small
  **per-voice ring buffer** (2 x 1-2 KB) that a slot loops over. The source offset is 32-bit, so length is
  unlimited. The 68000 at 11.3 MHz can afford a handful of these voices; the budget is measured and the
  driver caps them (default 4).
- **Streaming from main RAM:** the same ring, filled by the SH-2 from LWRAM (SH-2 DMAC or CPU copy; SCU DMA can't
  read LWRAM), for voice lines that don't
  fit sound RAM together with the stage's sfx.
- **One driver for game and FMV:** libyaul_cinepak uses `pcmsys` (ponut64, loaded as `SNDDRV.BIN`) for FILM audio.
  First choice: take pcmsys's open 68k source, which already has PCM slots, protected/volatile samples, ADX and
  a stream path, and add the ring voices above. Alternative: write `sr68k` (68000 C/asm; a 68000 GCC is on
  this machine as `~/gnu-tools/m68000/bin/m68k-atari-mint-*`, used freestanding) and reload the pcmsys driver
  around videos. Reloading costs a few ms, and no sfx play during FMV anyway.
- `aud.h` maps directly: `aud_sample_pack/file` → a bank entry; `aud_keep`/`aud_prefetch` → resident vs.
  per-stage; `aud_play/set_gain/stop/playing` → driver mailbox commands (volume/pan per voice). The core's
  `audio.c` (sfx table, variants, retrigger windows, voice slot, fades) stays unchanged.
- **Sound RAM budget (512 KB):** driver + mailbox ~16 KB, rings ~16 KB, a resident bank (UI, hero grunts
  and shots: ~120 KB) and a per-stage bank (~300 KB) loaded at stage start. All game audio is **~82 s**
  (26 s pack sfx, 19 s voices, 1.5 s turbo, the space/ramrod/power WAVs), of which the power clips' 24 s go
  into their FILM files. The builder picks the rate per sample (11/16/22 kHz) and 8-bit PCM vs 4-bit ADPCM by
  a loss measure, and fails if a stage's bank doesn't fit.

---

## 8. CPU: no FPU, two SH-2s

### 8.1 Bring-up first, then measure

Build the core as is (GCC soft-float from libgcc), with the null renderer (`platform/null`), and run level 1 in
mednafen headless with a scripted run. The kit's **per-instruction coverage** of the master SH-2 gives exact
call counts for `__addsf3`/`__mulsf3`/`__divsf3`/`floorf`/`sinf`…, and per-function cost. That tells how far
fixed point has to go. Expect soft-float at ~50-150 cycles per operation against ~477k cycles a frame.

### 8.2 Fixed point in the shared core

- A `fx` type (int32, 16.16) with `fx_mul` (SH-2 `dmuls.l`), `fx_div` (the DIVU unit), `fx_sin/cos` (LUT, 4096
  steps), `fx_atan2`, `fx_hypot` (no libm in loops; the Dreamcast lesson was that `hypotf` there was a real call).
- Convert module by module, in the order the profile gives. Likely order: `physics.c`, `character.c`, `enemies.c`,
  `bullets.c`, `player.c`, the camera, then `mode7.c`/`ramrod.c`/`space.c`. `menu.c`'s floats run once a frame
  and may stay.
- **Shared, not a Saturn fork:** the conversion goes into the core so PC, Windows and Dreamcast run the same
  arithmetic. Behaviour is checked against the current float build with the existing tools: the same
  `SABER_SCRIPT` input on both, `SABER_TRACE` positions/states compared frame by frame within a tolerance, and
  `tools/xvfb_record.py` recordings for feel. The RE constants (gravity 480, jump −290, speed 100, the level
  collision) must keep their exact results.
- `floorf` on positions (125 uses) mostly becomes `>> 16`.

### 8.3 The slave SH-2

Master: game update, input, audio commands. Slave, via libyaul's `cpu_dual` master→slave calls: building the next
VDP1 command list from the frame's draw calls (the backend records draws into a compact list; the slave turns it
into commands and palette/cache work), tile-streaming decode and uploads, LZ4, Mode-7 coefficient tables,
Cinepak decode. Mind the cache: shared buffers are read through the uncached mirror or purged
(`cpu_cache_purge`), as the examples do.

### 8.4 Memory diet (2 MB total)

- `Enemies` (215 KB) and `Game` (383 KB): size arrays to what a stage actually spawns (measure maxima on PC
  with a `SABER_TRACE` counter), and keep the per-stage world structs (`Mode7`, `Space`, `Ramrod`, `Forest`, `Night`)
  in one union-like arena that is reset per stage.
- Level data: LEVL maps are u32 cells (level 1: 156k cells = 623 KB raw). On Saturn they become the plane
  streams of 4.3 (compressed). Collision (2 x cols x rows bytes, level 1 = 24 KB) stays as is.
- HWRAM (1 MB): code (estimate 350-450 KB at `-O2`, check with `-Os` for cold modules), data/bss, stacks, the
  hot per-frame structures, the VDP1 command double buffer. LWRAM (1 MB): the stage arena (compressed plane
  streams, compressed sprite sheets, level objects, collision, dialog scripts), the FILM decode buffer during
  videos if option (b) of 6.1 is taken.
- `PLAT_LOW_MEMORY` and `PLAT_BAKED_ASSETS` already exist and are set for Saturn too.

### 8.5 The LWRAM / DMA rule

**SCU DMA cannot access LWRAM** (a hardware limitation). The rule: anything that SCU DMA moves to VDP1, VDP2,
CRAM or sound RAM starts in HWRAM (or the CD block/A-bus). LWRAM holds data that a CPU reads: compressed
stage data, level objects, collision, scripts, compressed sprite sheets, the Cinepak stream. Its paths to
hardware are:

- **CPU decode → HWRAM staging → SCU DMA** (the default for tiles, sprite frames, palettes; staging buffers
  of ~16-32 KB in HWRAM, double-buffered so the slave decodes while the DMA runs);
- **SH-2 DMAC** (2 channels per CPU; it can read LWRAM) for bulk moves where staging would double the cost,
  e.g. the Cinepak frame (6.1 b) or sound streams;
- **CPU writes straight to VRAM** for small or scattered updates (pattern-name columns, single cells).

The DMA helpers in `platform/saturn` check the source address (LWRAM = 0x002xxxxx / 0x202xxxxx) and fail
loudly in debug builds, so a wrong source shows up at once instead of as garbage in VRAM.
LWRAM is slower for the CPU too: the slave's LZ4 decoder reads LWRAM sequentially (good for its bus
  pattern) and writes HWRAM; hot per-frame structures never go to LWRAM.

---

## 9. Storage, disc and loading

- Nothing reads the original `.pck` files on the console. `tools/saturn/build_disc.py` works like the Dreamcast's
  `build_disc.py` and reuses `tools/dc/texprep.c`: the game's own `gfx.c` walks every pack sprite/tile bank/font.
  It writes packs in the same HEADLIST format (`pckwrite.py`, LZ4 blocks already supported: dir type
  `<type>+lz4`, `pack.c` decodes them with `lz4_mini.h`):
  - `SAT_GFX.PCK`: `SAT1` blocks (cells/sprite parts + palettes, 4bpp/8bpp), per stage;
  - `SAT_STG<n>.PCK`: the plane streams, the layer plan, line-scroll tables, collision, objects;
  - `SAT_SND.PCK`: sound banks (resident + per stage);
  - `FILES.PCK`: text, scripts, level files, the few images the game reads as pixels (as on Dreamcast);
  - `*.CPK`: FILM videos; CD-DA tracks for music.
- ISO 9660 file names 8.3 upper case; libyaul `cdfs` for the file list, `cd_block_sectors_read` for reads.
- A stage load reads 1-2 files sequentially (~300 KB/s → ~2-3 s for a stage's ~600-900 KB compressed), then
  LZ4 decodes into VRAM through HWRAM staging (8.5); compressed data the game keeps stays in LWRAM. The title card animation can run during decode if the slave does the decoding.
- `SABER_READLOG`-style logging of every read goes to the log ring (section 11).
- IP.BIN: `IP_AREAS`/`IP_PERIPHERALS` as in the VN game's Makefile; title "SABER RIDER"; the 1st read at
  0x06004000.

---

## 10. Input

- Saturn pad via `smpc_peripheral_*` (as in the VN game): D-pad moves, **B** jump, **C** shoot, **A** power attack
  (mirrors the six-button layout's natural thumb row; also Y/Z/X as alternates), **L/R** aim/turbo, **Start**
  pause. OPTIONS > CONTROLS remapping (already in the core) works unchanged.
- The 3D Control Pad's analog stick is optional (read as digital first).
- **A+B+C+Start** resets to the Saturn system menu (the platform convention; it mirrors the Dreamcast's reset combo).
- Debug switches: `SABER_*` lines from a `SABER.ENV` file on the disc (`plat_getenv`), as on Dreamcast.

---

## 11. Testing with the mednafen headless kit

- **Patch the kit for input.** Today `SetupDefaultInputs()` zeroes every port's input buffer and nothing writes
  it again. Add a protocol command `pad PORT HEXBYTES` that writes into the `MDFNI_SetInput` buffer
  (Saturn digital pad = 2 bytes, active-low bits), plus an MCP tool for it. With `run N` this gives scripted
  input with frame accuracy, the equivalent of the Dreamcast's `replay_input`. Keep the change in the kit's
  `headless-debug.patch` and rebuild with `build-headless.sh`.
- **Log ring.** The Saturn build keeps `printf`-style output in a RAM ring at a linker-exported symbol. The harness
  reads it with `mem_read` after each `run` and prints it. That replaces the Dreamcast's serial console.
- **Harness:** `tools/saturn/mednafen_run.py` (load cue → apply `SABER.ENV` → per-frame pad script → screenshots
  every N frames → `wav_start/stop` → log ring dump), and a frame tiler like the one used for Xvfb recordings
  (`fps=2,scale=320:-1,tile=6x4`) for comparing against the PC build frame by frame.
- **Performance:** `SABER_PERF` counters come from the SH-2 FRT (per-phase cycle counts written to the log ring).
  Instruction coverage gives the hot spots. Mednafen's Saturn timing is good but not exact, so the VDP1 draw
  time and cycle-pattern mistakes need a real console check at milestones (CD-R or ODE, if one is available).
- **Firmware:** `~/.mednafen/firmware/sega_101.bin` and `mpr-17933.bin` are present; the kit reuses them without
  touching the normal mednafen config. Use `--base` for parallel runs.

---

## 12. Build

- `Makefile.saturn` on libyaul's `build.pre.mk` / `build.post.iso-cue.mk` (`YAUL_INSTALL_ROOT=~/.local/x-tools/sh2eb-elf`):
  `SH_SRCS` = `src/*.c` + `src/platform/saturn/*.c` + `src/platform/common/*.c` (as needed) + the Cinepak player
  files; `-DPLAT_SATURN -DPLAT_LOW_MEMORY -DPLAT_BAKED_ASSETS`; `-O2`, with `-Os` for cold modules if code size
  bites.
- `make -f Makefile.saturn` → ELF; `make -f Makefile.saturn disc` → `build/saturn/saber_rider.cue` + ISO + WAV
  tracks (runs `tools/saturn/build_disc.py`); `rebake`/`repack` like the Dreamcast targets.
- `tools/release.sh --saturn` → `release/saber_rider-saturn-<date>-<commit>.zip` (cue/bin + README), refusing to
  run with a `SABER.ENV` in the stage dir, as for the Dreamcast.
- New files: `src/platform/saturn/{main_sat.c, plat_sat.c, input_sat.c, render_sat.c (VDP1 lists, CRAM, caches),
  vdp2_planes.c, floor_rbg0.c, aud_sat.c, video_cpk.c, cd_sat.c, log_sat.c}`, `src/fx.h`,
  `src/platform/common/plane_soft.c`, `tools/saturn/*`, `third_party/libyaul_cinepak` (vendored, like the
  DCMV player) and the sound driver sources.

---

## 13. Milestones

Each milestone ends with a mednafen run the harness can repeat, and a commit.

| # | Milestone | Done when |
|---|---|---|
| 0 | **Skeleton**: Makefile.saturn, `platform/saturn` with null render/audio, log ring, mednafen `pad` patch + harness | boots, runs the menu logic headless, input script reaches level 1 (logged states) |
| 1 | **CPU reality check**: level 1 update under soft-float, coverage profile; struct diet; memory map | a report: cycles/frame per module, RAM map; fixed-point scope agreed |
| 2 | **Fixed point** in the core (shared), trace-compared against the float build | PC traces match within tolerance; level 1 update < 60 % of a frame on the master |
| 3 | **Asset baker v1**: 4bpp/8bpp policy + palette packing + LZ4, reports and contact sheets | level 1 + front-end baked, loss/CRAM/VRAM report reviewed |
| 4 | **VDP1 + front-end**: sprites, fonts, dialog, menus, splashes, title, char select as bitmaps/sprites | front end matches PC screenshots at 320x224 |
| 5 | **Platform renderer**: layer planner, VDP2 planes, tile streaming, sprite priorities | level 1 playable at 60 fps, visually matched to PC along a camera sweep |
| 6 | **Audio**: driver (no 64 KB cap), sfx banks, CD-DA with DSP fades | level 1 with full sound; long sample (> 64 KB) test passes |
| 7 | **FMV**: intro, briefing (Plan A), power clips with CD-DA resume | full demo flow like PC, splash to MISSION ACCOMPLISHED |
| 8 | **Stages 3, 4, 5** (same renderer; night colour offset, forest front walls, Dark April palettes) | all three playable and matched |
| 9 | **Mode 7**: stage 2 on RBG0, sliding window, hashed sand; stage 6 cockpit | both playable; 60 fps target, 30 fps floor |
| 10 | **Space shmup** (stage 6 final phase) | playable to the credits |
| 11 | **352x224 wide**, clock-change re-init, 224-line layout audit closed, backup-RAM options | both ratios through the whole game |
| 12 | **Hardware pass + release**: real console test, release zip | the whole game on real hardware |

---

## 14. Risks and open questions

**Decisions for you:**

1. **Fixed point in the shared core** (recommended, one code path everywhere, verified against the float build) or
   a Saturn-only copy of the hot modules?
2. **Power clips**: stop CD-DA and resume at the saved position (proposed), or replace the clips on Saturn with
   a still + voice to keep the music running?
3. **Briefing**: Plan A (full 24bpp screen with the room) is proposed. Is Plan B (video + text only) acceptable
   if A doesn't make the frame budget?
4. **Frame rate floor** for Mode 7 / space: is 30 fps (two logic steps a frame) acceptable if 60 isn't reachable?
5. **RATIO switch** if the clock change forces a full re-init: boot-time choice, or a re-init from OPTIONS
   (a second of black)?

**Technical risks:**

- Soft-float cost is the biggest unknown. Milestone 1 decides the fixed-point scope before any renderer work.
- VDP2 cycle patterns: 4 NBGs, or RBG0 + 2 NBGs, with line scroll must follow the slot rules; mednafen is
  forgiving in places, so check on hardware.
- CRAM (2048 colours) across 4 planes + sprites per stage; the baker's report catches it early. The fallback
  is 8bpp banks shared by several layers, or fewer palettes per layer at a small loss.
- Level layers that neither merge nor band cleanly (overlapping far layers with different parallax): the
  dominant-layer line scroll is an approximation. Show the builder's report for the worst stage before
  accepting it.
- The 64 KB sample cap and 68k decode budget: the ring-voice design removes the cap. The number of
  simultaneous compressed voices is the new limit.
- CD-DA seeks on track changes; FMV mid-level stops the music (6.3).
- Stage 6's space shmup bullet counts and Mode-7 billboard counts on VDP1; measure the PC build's worst frames.
- The 4 MB RAM cartridge is explicitly out of scope for now. If added later it would hold whole stages
  uncompressed and remove most streaming, but nothing in this plan depends on it.

---

## Appendix: measurements behind this plan

- Level 1 (`levels.pck` 12DAD1A7): 14 layers (11 tile maps), 9953x256 px, parallax per layer as in 4.2.
- Tile banks: 21 banks / 5176 tiles; level 1's banks ≈ 2.6 MB at 8bpp. Exact 4bpp: 86.2 % per 16x16 tile,
  96.2 % per 8x8 cell. Whole-frame 15-colour quantisation of character sheets: 23-32 dB PSNR (not acceptable).
- Music: 18 tracks, 1335.7 s. Pack sfx: 33 files, 26.2 s (longest 4.66 s). Voices: 30 files, 18.9 s. Other WAVs:
  space 8, ramrod 6, sfx 2, power 3 (the power clips' audio 10.9 s + 12.9 s).
- Videos: intro 768x384 60.3 s (Vorbis stereo), briefing 768x312 6.1 s, heroes 768x384 7.7 s (unused), power clips
  320x240 24 fps.
- Core: `sizeof(Game)` 383 KB, `sizeof(Enemies)` 215 KB (32-bit build); stage 2 material map 1 MB.

---

## 15. Progress

### M0 — skeleton (done)

`Makefile.saturn` (libyaul rules, ISO + CUE from `build/saturn/stage`), `src/platform/saturn` (CD-backed `fopen`,
RAM log ring, pad, FRT timing, a libc shim for what libyaul lacks), `src/platform/null` (render / audio / video that
do nothing), `tools/saturn/build_disc.py` + `mednafen_run.py`. The mednafen kit got a `pad` command, disc insertion on
load and a fixed `regs` command. A pad script reaches level 1.

### M1 — CPU and memory reality check (measured in mednafen, level 1, `tools/saturn/bench_level1.env`)

| | before | after |
|---|---|---|
| soft-float | libgcc `fp-bit.c`: 65 % of all instructions in the first in-level profile | `platform/saturn/softfloat_sat.c` (bit-exact vs. the FPU, DIVU for division): draw-side core time 54 → 30 ms/frame, update ~7 → ~5 ms/step |
| level 1 load | ~50 s (libyaul re-seeks on every read: 420 reads, 17 KB/s) | ~8 s (the drive keeps streaming: 43 seeks) |
| update, level 1 (5-8 enemies) | | 3-4.6 ms/frame average (< 30 % of a frame), rare spikes to ~15 ms (to look at) |
| `sizeof(Game)` | 383 KB (`Character` held its type's 2.9 KB of tables in each of 64 enemy slots) | 162 KB (shared `CharDef`) |

RAM map, level 1, null renderer: high work RAM = BIOS/boot stack 16 KB, code 333 KB, rodata/data 58 KB, BSS 329 KB
(Game 162 KB, stack 48 KB, sfx overrides 24 KB, batch 16 KB, libyaul pool 40 KB), heap 193 KB free. Low work RAM:
696 KB used (the LEVL block alone is 623 KB), 327 KB free. The remaining draw cost is the per-tile layer loop, which
the VDP2 planes replace (M5).

### M2 — fixed point (shared)

`src/fx.h` / `src/fx.c`: 16.16 `fx`, binary angles, `fx_mul/div/muldiv` (the SH-2's divider on the Saturn),
table sine/cosine, `fx_atan2`, `fx_sqrt`/`fx_hypot`; `make test` checks it (and the soft-float) on the host. Used
by every platform. First user: `physics.c` makes its collision decisions in fx. `SABER_TRACE=3` traces every step
precisely; the four heroes' level-1 traces (3286 steps, `bench_level1.env`) are identical to the float build.
Further modules move to fx where a stage's profile shows float cost (Mode 7, cockpit, space: M9/M10).

Decisions taken for the open questions of section 14 (defaults of this plan, revisit on request): power clips stop
CD-DA and resume; briefing Plan A with Plan B as fallback; 30 fps accepted as the floor for Mode 7 / space; RATIO
switch by re-init from OPTIONS.

### M3 — asset baker v1 (`tools/saturn/satbake.py`, `build/saturn/bake.log`)

Every texture is cut into the rectangles the game draws out of it (sprite frames, cblock tiles, glyphs, atlas entries),
each unit's opaque box into VDP1-sized parts (≤ 504 px wide, ≤ 255 rows, ≤ 32 KB raw), each part stored LZ4 where that
is smaller, as:

- **4bpp + colour table** (VDP1 LUT mode, RGB entries) where a band of rows has ≤ 15 colours (exact);
- **8bpp palette** (VDP1 colour-bank mode through VDP2 CRAM) when the texture has ≤ 255 colours, or quantises to 255
  (weighted k-means from a median-cut start) at ≥ 40 dB PSNR against its 15-bit colours;
- **16bpp RGB** otherwise.

| | 16bpp parts (raw) | 8bpp parts | stored on disc |
|---|---|---|---|
| before (4bpp / 16bpp only) | 10 MB | — | 4.4 MB |
| now | 4.2 MB | 2.7 MB | 4.2 MB |

Every pack texture but one (`0C5FAD65`, 2126 colours) is now 4bpp/8bpp; the spiral (512x512, 31 colours) went from
464 KB of 16bpp (more than VDP1's texture memory) to 256 KB of 8bpp. Quantised: forest/play, the ramrod cockpit and
sky, space planet and far ship, three stage-3 victory paintings (40-49 dB). Left 16bpp (34-39 dB): the other victory
paintings (202 KB each, alone on their screen), `mode7.png` and `ramrod/atlas.png` (per-frame palettes for those two
come with M9/M10). Not done yet: per-cell palette packing for the VDP2 planes (with M5), contact sheets.

### M4 — VDP1 + front end (`src/platform/saturn/render_sat.c`)

The front end runs from the splashes through notice, title zoom, main menu, OPTIONS (+ backer credits), briefing
(text box, hero pieces) and character select to the level's LOADING screen, checked in mednafen screenshots
(`tools/saturn/mednafen_run.py --shots`). What it took:

- **VDP1 frame change**: `vdp1_sync_interval_set(-1)` (variable). The former AUTO mode (0) swapped the framebuffers
  every field and cut off frames VDP1 needed longer for: the main menu lost the lower logo and its text, every frame.
  The front end runs at ~34 fps in mednafen (the game steps by elapsed fields, so its speed is unchanged).
- **CRAM palettes**: CRAM mode 1, 32 granules of 64 colours; an 8bpp texture's palette gets a 64/128/256-colour bank
  (VDP1 colour-bank modes 2/3/4) while it is drawn, reused LRU once neither the frame being built, drawn nor shown uses
  it. Colour mod on an 8bpp texture = a tinted copy of its palette (exact; the OPTIONS spiral at 40 %).
- **Palette pixels can't be blended by VDP1** (half-transparency over them draws opaque, checked in mednafen's
  `vdp1_common.h`): 8bpp parts use mesh for alpha, and so does a translucent polygon once palette pixels were drawn.
  A translucent **full-screen fill that ends the frame** (the core's fades) becomes the **VDP2 colour offset** (plan 4.4).
- **Memory**: a destroyed texture's VRAM / CRAM stay reserved until the frames in flight are done; pack block reads
  can evict textures (core: `packs_set_evict_hook`, gfx.c `evict_any`), so level 1's 700 KB LEVL block loads after
  the menus.
- Debug: `SABER_RTRACE=n` logs every draw of frames n, n+10 … n+50 (parts, formats, VRAM, tint); a failed VRAM
  allocation dumps the cache per texture.

Open for M4: a side-by-side against PC screenshots (no SDL3 in the cloud session this was done in); VDP1 draw time per
screen; the briefing/intro videos (M7). **Level 1 does not fit yet with the VDP1 renderer**: the tile banks' RAM copies
next to the 623 KB LEVL map exhaust both heaps (`D8B018EE`: 301 KB). That is M5 (planes streamed from compressed
columns, LEVL replaced by the plane streams, plan 4.3 / 8.4).

Testing notes: the disc's area code includes North America, so mednafen picks the US BIOS; with only `sega_101.bin`,
set `ss.region_autodetect 0` and `ss.region_default jp` (the headless kit's `set` command, kept in
`mednafen-headless.cfg`).

### M5 — platform renderer (in progress: level 1 on VDP2, 30-60 fps)

**Planes** (`tools/saturn/layers.py`, `src/platform/saturn/vdp2_planes.c`, `build/saturn/planes_12DAD1A7.png` = PC
render vs. planes at five camera positions). Level 1's 11 tile layers on NBG0-3:

| Plane | Contents | Rate |
|---|---|---|
| NBG0 (line scroll, prio 2) | band 0-79 px: SkyBG (static); band 80-175: Far + Mountains merged, the sky as one colour a row | 0 / 0.3 (Far 0.2 on PC) |
| NBG1 (3) | NearMountains | 0.4 |
| NBG2 (4) | MidBG + Cars MidBG re-anchored per car (≤ 5 px off at the screen edges) | 0.875 (cars 0.9) |
| NBG3 (5) | Playfield + Platforms + Cars | 1.0 |
| VDP1 backdrop (prio 1) | sky rows 80-95 (the planet's lower edge the mountain band would cut), a palette sprite | static |
| VDP1 (6) | sprites, ForegroundStuff (1.0), ForegroundStuf2 (1.2) | |

8x8 4bpp cells deduplicated across flips: 11807 cells, 368 KB, all resident (no streaming needed); 96 palettes by
lossy clustering (37.5 dB on the 5-bit scale; exact packing needed 455, CRAM holds 128); u16 names in LZ4 column
chunks (103 KB) streamed into one 64x64 page per NBG. The level block ships without the planes' maps (171 KB instead
of 700), in `stage.pck` (opened first); ForegroundStuff's tile bank is cut to its 27 tiles for VDP1 (11 KB instead
of 294). Level 1 in RAM after loading from the menus: 17 KB high / 163 KB low free.

Found on the way: libyaul's `vdp2_vram_control_set` resets CRAM to mode 0 (worked around); the VDP1 frame change
must be variable (AUTO cut off slow frames); the vblank erase of the variable change misses the bottom ~40 lines
(a transparent polygon clears the framebuffer while the planes are on); `DISP_NBGn` makes colour 0 opaque (use
`DISPTP`).

**Speed** (level 1, `bench_level1.env`, mednafen), before the slave SH-2 took the list building: 60 fps with 0-3
enemies, 30-60 with 5-9. Per frame: update 2-6 ms,
draw 7-13 ms (textured draws ~6 ms, the planes 0.8, the core's own drawing the rest). Done so far: an integer path
for unrotated textured draws (no soft-float), no libgcc variable shifts / divisions / soft-double compares in the
draw path or the soft-float (multiplies by powers of two; still bit-exact: `make test`), `floorf` & co. on the bits,
a division-free `sat_timer_us`, per-texture LRU stamps and grid lookups, gouraud tables uploaded once VDP1 is done,
the enemy spawner's integer early-out (exact). `mednafen_run.py --callers` lists the soft-float callers.

**The slave SH-2** (plan 8.3, `render_sat.c`): the core's draw calls are recorded on the master (a few stores each,
with the texture's colour mod / alpha / blend of the moment); at the frame end the slave replays them into VDP1's
list while the master runs the next frame, and the list goes to VDP1 at the frame end after (one frame of delay; the
planes, the colour offset and the back colour follow the list they belong to). Texture destroy / update wait for the
slave; a texture the frame being recorded drew is freed after that frame's replay. `SABER_NOSLAVE` replays on the
master (for comparisons). Level 1: the master's draw 5-7 ms (was 12-14), 1.00-1.03 fields a frame in most of the run,
1.1-1.6 with 7-8 enemies (the update, 3-5 ms, is the rest). The planes' palettes went from 96 to 64 (35.9 dB): VDP1's
8bpp sprites needed more than 8 colour banks in a level (April's power attack art found none).

Next for M5: the remaining soft-float in physics / animation (fx); sprites between planes (Hyperjumper's layers 4
and 7: palette sprites with priority bits); the 224-line framing (the bottom 16 px of the PC view are cut now); a
thin dark line at y 80 during the power attack's white flash (the backdrop strip's edge).
