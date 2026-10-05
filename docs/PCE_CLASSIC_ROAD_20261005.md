# PCE level 2: the classic racing road (2026-10-05)

The Wolf3D bitmap-tilemap floor (128 samples across, 19 strips, a new picture about every 4th frame) is gone. Level 2 now draws
its road the way Chase H.Q. does (`PCE/References/Chase H.Q. (USA).pce`; its VRAM holds one static road picture, and a raster
interrupt scrolls and re-selects it for every scanline). The sky is unchanged.

## How it works

* **One static picture** (`tools/pce/build_assets.py road_assets`): a straight road in perspective, 1024 dots wide (centre at
  dot 512), one image line for each scanline below the horizon (scanline 113). Edges are drawn at integer and quarter slopes
  (dots a line), so the sloped characters repeat from row to row: the whole picture is 148 characters at VRAM `$2000`.
  Kerb outside at 5.0 dots a line (120 units from the centre), asphalt to 4.25 (102, the source map's own widths), a white
  edge line, a centre dash.
* **Two copies of its BAT rows** (rows 0-11 and 12-23), the same characters with different palettes (sand/kerb/dash/asphalt
  colours of the two stripe phases). The 14 sky BAT rows are untouched; sky rows 62-63 (scanlines 112-127, formerly flat haze)
  hold the road's far rows in hazed palettes. The first copy follows scanline 127 by itself (BYR counts on from the sky's 384
  and wraps to row 0 at scanline 128).
* **Per-scanline raster** (`irq.S pce_hblank`): one RCR interrupt for each pair of scanlines from the end of scanline 111 to 221. It writes
  BXR from a table (the road's curve and the camera's offset) and BYR only where the stripe phase changes.
* **Tables** (`road_pce.c road_update`, banks `$6d` and `$6f`, every frame): the circuit's points ahead of the camera are rotated into
  the camera's frame (forward `f`, right `sd`); a point is on scanline `113 + 10080/f` and `sd*d/23.9` dots right of the middle;
  between two points the road's centre is a straight line in the scanline (exact for a straight piece), so each segment is one
  division and a run of additions. The stripe of a scanline is `((progress + 10080/d) >> 5) & 1` (32-unit bands from the
  distance along the circuit); the first 16 scanlines of the region never stripe, and rows 2-5 of the picture fade into the
  haze to hide aliasing. The tables live in the platform background cache array (`columns`, unused in the race), two buffers
  of three 128-byte pages (BXR low, BXR high, stripe copy); the VBlank handler latches the finished buffer (`pce_floor_pending`).
* **Pursuit**: a straight road along -y at x 4096 (the source's pursuit road) gives the points.
* **Steering**: the car keeps within 34 degrees of the road's direction (`race_pce.c MAX_OFF`), since a straight road in
  perspective is only right for a road seen head-on. The circuit's tangent is now 256 steps a turn (`export_track.c`).
* Cars, mines and shots are projected with the same camera (`race_draw_pce.c project_point`), which now is the live camera,
  not the camera of the last finished floor.

Removed: `floor_pce.c`, `floor_sample.S`, `floor_tables.py`, the 256 pair characters, the floor geometry archive, the 19-strip
interrupt.

## Stability pass (2026-10-05, second session)

Three faults of the first version, and what was done (the usual way retro racers handle them: Lou's Pseudo 3D page, Chase H.Q. itself):

* **Horizon too far.** The road went on to f = 1023 (scanline 113 + 9); the far knots swung by hundreds of dots. The road now ends at
  `FAR_F` = 560 (scanline 113 + 18) and fades into the haze: BAT rows 0-1 of the picture are full haze, row 2 mixes 60%, rows 3-4 40% and 20%
  (`build_assets.py road_assets`), and the stripes start at image line 24. Cars, mines and shots are culled at the same distance
  (`race_draw_pce.c project_point`), 560 units. The knot walk is 13 knots (every sample for 8, then every second one).
* **Wrap-round.** A scanline's BXR window is 512 dots of a 1024-dot picture: when the road's centre was pushed more than 256 dots off, the
  window ran over the picture's edge and showed the far kerb on the wrong side of the screen (the "hook" at the far right). Every knot's
  offset is clamped to +-250 dots (`X_LIMIT`, `dots_q4`), so BXR stays in 6..506 and, since a line is a straight interpolation of its
  knots, so does everything between them. (Chase H.Q. has the same wrap; this is the plain fix: keep the window inside the picture.)
* **Unstable road.** (1) The camera heading was the car's own, up to 34 degrees off the road, so steering swung the whole far road round the
  car. It is now the road's direction at the car, as in Chase H.Q.: `race_pce.c road_heading` (the circuit's tangent blended between its
  samples), and steering slides the road sideways instead. The car's own heading only moves the car. (2) `cam_c`/`cam_s` came from a table of
  256 steps a turn and were 7-bit: one step of the camera moved the far road 3-10 dots. They are now interpolated to 14 bits (`sine14`),
  held as a Q7 byte plus a remainder (`cam_c`/`cam_cl`), and the knot walk (`road_fill.S road_advance`) and the first knot's rotation keep Q14
  (`road_k`, 32 bits); the side offset is taken in quarter units. Cars use the same Q14 (`project_point`), so they sit on the road without
  shimmer. (3) `project` now finds the car's place along its circuit segment from the segment's real length (36-66 units, it was a fixed
  52), which made progress (and the stripes) jump at each new nearest sample.
* Measured (autopilot, `tools/pce/test_road.py`): the table value at scanline 30 moves by 1-2 dots a step where it jumped by 4-9 before.
  The loop runs at 19-20 commits a second in the same test (it was 22-23 with the less precise walk): the walk now uses eight byte products a knot.

Bank notes: `road_stripes` and `pce_road_z` moved to $6d, `story_graphics_restore` to $6e (`overlay_call(0x6e,..)`), `hurt_call` to $7c, `ground_call` to $78.

## Cost and results

* Measured in the accurate-core emulator with seven rivals and an autopilot: the loop completes about 22 times a second and the
  road changes every time (the old floor: 15 passes a second at 128 samples across). The road builder is about 50-60k CPU cycles a
  frame (11 knots, each 4 byte-by-byte products in `smul8`, one 16-by-8 division a segment, `road_fill` at about 90 cycles a
  line), the raster interrupts 8% of the machine, cars and HUD sprites the largest remaining share, and the simulation (every
  tick, catch-up after a slow frame) about a third of a 60 Hz frame a tick, mostly `__mulhi3` (`tools/pce/prof_report.py`
  summarises a `prof_dump` by function). The numbers to beat for 30 Hz are the sprites and `__mulhi3`, not the road.
* `tools/pce/test_road.py` drives the car round the circuit with an autopilot, takes screenshots, prints the tables and the refresh
  rate (`--profile` adds a cycle dump).
* Bank use: the knot arithmetic, its tables and `smul8` are in `$6f`, the table filler in `$6d` with the field and the sky loader.
* Not done: roadside scenery (posts, rocks) for a stronger sense of speed; a 30 Hz road needs a cheaper simulation tick and sprite
  pass first.
