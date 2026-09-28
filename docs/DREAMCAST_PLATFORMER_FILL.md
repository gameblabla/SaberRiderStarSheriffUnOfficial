# Dreamcast platformer fill reduction, 2026-09-28

Real hardware still slows down in the platformer stages at 640x480 and 832x480 after
61e8aa8. That change mostly helped Stage 1; 320x240 performs well. No Flycast run was
used to develop or validate this follow-up.

## Hardware basis

The local `DreamcastDevBoxSystemArchitecture.pdf` (also in
`dreamcast-docs/files/official/`) describes these relevant differences:

- **§3.1.1.9, printed p. 97:** opaque polygons resolve visibility before texturing.
  Punch-through requires texture processing through successive transparent layers;
  translucent geometry shades each visible layer. The previous renderer comments
  incorrectly described OP and PT as equally cheap.
- **§3.4.3 and §3.4.3.1, p. 104:** punch-through reads texel alpha, works through groups
  of polygons in the ISP cache, and can lose efficiency with overlapping transparent
  texels. Moving a whole tile from TR to PT does not remove the cost of its holes.
- **§3.4.7.2.2, p. 114:** bilinear filtering samples adjacent texels, including outside
  a subrectangle in an atlas. The new partitioning therefore applies only to nearest
  sampling, where the existing alpha blocks describe exactly the source region.
- **§3.6.1.4, p. 140:** the ARGB8888 palette performance penalty applies with filters
  other than point sampling. Stage tile sheets already use nearest sampling; switching
  all palettes to a lower colour precision would not address that path. The same
  restriction appears in [KOS's palette API documentation](https://kos-docs.dreamcast.wiki/group__pvr__pal__mgmt.html).

KOS's local `kernel/arch/dreamcast/hardware/pvr/pvr_misc.c` updates
`rnd_last_time` from render-start/render-done events. `reg_last_time` measures TA
registration (including time spent submitting), and `vtx_buffer_used` reads parameter
memory consumption. These provide useful console measurements, unlike host GPU timing.

## Change

The earlier code classified each complete 16x16 tile using its four 8x8 alpha blocks.
One partially transparent texel could put the entire tile in TR. One transparent
texel could put the entire tile in PT, including fully opaque and empty subregions.

`r_tex_batch` now partitions mixed, aligned 16x16 tiles by those existing blocks.
Equal neighbours merge into rectangles, so a uniform tile still needs one quad and
a mixed tile needs at most four. Empty regions are omitted, solid regions use OP,
cutouts use PT, and partial alpha uses TR. Alpha modulation and additive blending
still select the appropriate blended header. Source UVs, horizontal flips, clipping,
colour modulation and painter order are retained. Each new primitive gets the same
depth-order treatment as existing primitives. Plain copies, bilinear draws, sources
without an alpha map and other tile sizes retain their existing path.

The OP staging buffer grows from 160 to 256 KiB, costing 96 KiB of additional main RAM.
PT stays at 160 KiB; the PVR's 512 KiB parameter buffer is unchanged. This keeps the
extra solid rectangles from falling back into TR in the wide modes. The existing
allocation/overflow fallback remains available.

`SABER_PERF=2` now also prints peak completed PVR render time (`gpu`), TA registration
time (`ta`) and parameter usage (`vtx`) over 60 submitted frames. These peaks can come
from different frames. The list area counters measure submitted bounds, including
geometry subsequently rejected by depth; they are not shaded-pixel counts.

## Verification and measured submission reductions

Commands run:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -f Makefile.dc -j4
python3 tools/dc/tile_regions_test.py --tex-pack build/dc/stage/data/tex.pck
make -f Makefile.dc repack
git diff --check
```

The host test compiles the actual C partitioner and checks all 256 combinations of
four alpha classes for complete, nonoverlapping coverage. It decodes the staged PVT1
tile sheets, verifies the alpha classes and nearest texture coordinates of all 4,770
used forest tiles and 5,347 used lab tiles at 1x, 1.5x and 2x, with mirroring and
clipping, then sweeps each route in 8-pixel camera steps. This is a geometry/asset
check, not a PVR rasterizer or timing simulation.

Mean submitted PT+TR area, in logical screens, for the tile layers only:

| Stage | Logical width | Previous | Split tiles | Reduction |
| --- | ---: | ---: | ---: | ---: |
| 4 | 320 | 1.417 | 0.901 | 36% |
| 4 | 416 | 1.415 | 0.899 | 36% |
| 4 | 426 | 1.414 | 0.899 | 36% |
| 5 | 320 | 0.280 | 0.133 | 53% |
| 5 | 416 | 0.280 | 0.132 | 53% |
| 5 | 426 | 0.280 | 0.132 | 53% |

At Stage 4's opening, 320-wide tile coverage changes from 0.913 PT + 0.517 TR screens
to 0.578 PT + 0.338 TR. At 416 wide it changes from 0.951 PT + 0.554 TR to
0.604 PT + 0.369 TR. These numbers use the baked palette alpha, which can differ from
the PNG sources after quantization.

The largest conservative OP/PT staging bounds across the route samples are
202,080 / 102,560 bytes for Stage 4, and 176,960 / 31,680 bytes for Stage 5. This assumes
a new 32-byte header for every 128-byte quad. It excludes actors, HUD and effects;
`moved to tr (ram full)` and `vtx` remain the full-scene checks on the console.

## Console comparison

The rebuilt `build/dc/saber_rider.cdi` uses the new default and existing staged media.
For a diagnostic image, put these lines in `build/dc/stage/saber.env`, then repack:

```ini
SABER_LEVEL=4
SABER_PERF=2
```

Compare the same location/action in 320x240, 640x480 and 832x480, including scrolling,
foreground foliage, combat and the boss. Use `SABER_PVR_TILE_SPLIT=0` and repack for a
comparison with whole-tile classification; both builds retain the larger buffer.
Do not set `SABER_PVR_TR` for this comparison, because it forces every primitive into TR.
Repeat in Stage 5 and check mirrored tiles in Stage 3. Serial logging itself can
perturb frame cadence, so also compare normal builds without `SABER_PERF`.

Console FPS and rendering correctness remain unverified here. The change trades
additional small polygons and 96 KiB of RAM for fewer PT/TR texture reads. Its net
timing benefit, and whether Stage 4 now meets a 60 Hz deadline, need a real console.
