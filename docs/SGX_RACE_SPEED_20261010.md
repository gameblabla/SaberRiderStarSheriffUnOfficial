# SGX race speed, 2026-10-10

The race from commit `b295bb0` measured 20.75 road updates/second in the
12-second stage-2 autopilot fixture. The optimized version measures 30.08 road
updates/second and 30.00 displayed SAT generations/second: about 45% more road
updates. This is an improvement, not a claim of constant 60 fps.

The accurate Mednafen core ran with the hardware sprite limit enabled. The
supplied Chase H.Q. ROM and start-of-race state produced a changed road-only crop on
107 of 119 intervals of a 120-video-frame accelerating sample. Uniform road
areas can repeat between images; this crop measurement does not establish a
constant 60 Hz internal generation rate. It is distinct from Saber Rider's
native generation counters.

## Changes

- Bake 4,096 reference-camera road curves from the existing 256-point track.
  Each 128-byte record contains 48 pixel centres and its reference camera pose.
  The 512 KiB table makes the stage-2 archive 1,820,672 bytes, below the
  1,966,080-byte scene limit. Runtime stages two records in the unused column
  cache, interpolates four progress bits and applies Q2 lateral and small-yaw
  corrections. The raster IRQ and short transfer deadlines are unchanged.
- Keep `road_reference` as a volatile native comparison switch. The old knot
  builder remains available; normal SGX gameplay uses the lookup builder.
  The straight pursuit skips curve reads and interpolation.
- Split the borrowed race quarter-square table into low/high byte planes and
  use resident assembly products. Race products no longer switch MPR6 or
  build word-index pointers in C. Signed word/byte products preserve full
  32-bit results; the shared movement helper also preserves its high byte
  for escort and leader steps above 1,023.
- Replace repeated turbo-limit arithmetic, boost-clock modulus and HP/digit
  HUD divisions with exact constants or bounded tables.
- Enable the existing native 32x16 cell packing for SGX race sprites. Pattern
  bytes and upload sizes stay the same; 468 paired cells replace corresponding
  pairs of SAT entries. Native screenshot comparison against restored 16x16
  descriptors is pixel-identical in the halted race fixture.

The curve lookup and race assembly fast paths are **SGX-build changes**. The
ordinary PCE build receives the shared turbo/HUD changes and still uses its
original road builder. Both images build successfully. The ordinary PCE native signed-byte multiply
check also passes all 65,536 pairs.

## Checks and limits

Passed: exhaustive native signed-byte multiplication (65,536 pairs), all 256
track reciprocals, signed word/byte boundary and random cases, and movement
products including pursuit steps; native road comparison at 160 fixed poses;
12-second autopilot cadence; opening/finish/boss/victory sky restoration; boss
CD-DA; the perspective-size sweep; and the 32/16-cell native pixel comparison.

At the 32 sampled reference poses, the lookup road matches the old displayed
centres exactly. With lateral offsets +/-120 units or yaw offsets +/-256 angle
units, the near road differs by at most 5 dots. The camera correction is an
approximation: distant bends can differ by up to 85 dots from the old renderer,
whose world-side knot clamp reacts differently to camera translation. The
lookup is not a pixel-equivalent replacement for every off-centre pose.

The final timed run recorded one essential overflow event; the baseline had
two. Different presentation cadence changes the autopilot trajectory, so this
is not proof of identical admissions at every matched world position.

The remaining profile includes road interpolation/fill (11.4%), generic sprite
emission (10.0%), raster callbacks (8.6%), exact sprite-line accounting (4.3%),
entity projection and simulation. Achieving 60 fps still needs further work.
Full campaign/full SGX suite and physical hardware were not tested in this task.

Review artifacts: `artifacts/race-speed/{before-report,after-report,geometry,
race-verification,chase-reference}.json`. Disc: `build/sgx/saber_rider.cue`.
