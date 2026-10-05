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

## Raster glitches while standing still (2026-10-05, third session)

Standing on the grid (no steering, the tables identical from frame to frame) the road still flickered: now and then one or two
scanlines of the road were shifted or showed the wrong stripe colour for one frame (about 4% of the frames; a screenshot diff of
consecutive frames showed single full-width rows changing). The tables were stable, so the fault was the interrupt timing.

* **Diagnosis.** An instrumented copy of the emulator (log of every RCR interrupt: assertion time, latency, the instruction it
  interrupted, and the scanline/phase of each BXR/BYR write) showed the handler normally writes BXR about 60 dots into the line after the
  interrupt, which leaves about 300 CPU cycles of slack before the next scanline latches BXR/BYR. Whenever the interrupt was held back
  by more than that, scanline p got the previous pair's values (a one-line glitch). Every long delay came from block transfers,
  which the HuC6280 does not interrupt: `arcade_tai` (the piece list of a sprite, up to 255 bytes at 6 cycles a byte, 600-1500
  cycles), the 64-byte TIA bursts of `arcade_vdc_copy` (VRAM uploads) and `hud_copy` (SAT copy, 401 cycles each).
  (Padding the handler by 150 NOPs changed nothing and 180 NOPs shifted every line by one: that is how the 300-cycle slack was measured.)
* **Fix.** `arcade_tai` now moves its length in bursts of at most 16 bytes (113 cycles) with interrupts open between them, and the TIA
  bursts of `arcade_vdc_copy` and `hud_copy` are 16 bytes instead of 64. Late interrupts fell from 94 in 115,000 (4% of the frames
  glitched) to 3 in 132,000 standing still, and from 48 to 8 in 200,000 while driving the circuit; 900 consecutive standing frames show no
  changed road row at all (`glitch.py`-style screenshot diff; before: 6-16 glitch frames per 150-300).
* **Cost.** The race loop runs 17.9 times a second in `tools/pce/test_road.py` (19.1 before): the extra burst set-up is about 45 cycles a
  16 bytes. Remaining long delays are rare BIOS/CD handlers and the PCM timer handler stacking onto a burst.
* Rule for new code in the race: no uninterruptible block move longer than about 16 bytes (the RCR handler's slack is ~300 cycles,
  and a PCM timer interrupt can queue in front of it).

## Driving onto the sand, and the turbo + fire glitches (2026-10-05, fourth session)

* **Turbo + fire glitches.** Firing calls `audio_pcm_play` (`audio_pcm.c start`), which kept interrupts off for the whole of its
  compiled body (about 500 cycles: table lookups and sixteen volatile stores), so the road's raster interrupt arrived a scanline late whenever a
  shot was fired (the instrumented emulator showed 32 late interrupts, all just after `overlay_call` returned from it, in 400 frames of
  turbo + fire). The sample's numbers are now read and the voice record written with interrupts on (the voice is switched off for that
  time, which is all the timer handler looks at), and only the PSG set-up is atomic: 28 late interrupts in 212,000 became 6.
* **The road is half as wide.** The picture was built for a focal length of 421 dots: the kerb (120 units) was 5 dots a line, wider than the
  picture's 1024 dots from the 100th line down, so there was no sand to show at the bottom of the screen and the last session's +-250 dot
  limit pinned the road in place as soon as the car left its middle. The picture's slopes are now halved (kerb 2.5, white line 2.25, asphalt
  2.0 dots a line; `build_assets.py road_assets`), and so is the world-to-dots scale everywhere: `road_pce.c dots_q4` (85/1024), the
  cars' and shots' `project_point` (85/256) and the cars' baked size (`rows` x 0.59). The road is narrower on the screen but has sand
  for 400 dots either side of its kerbs at the bottom line, and the BXR window may run past the picture's edge (it wraps into the
  other edge's sand), so `X_LIMIT` is gone.
* **The wall.** The window shows the far kerb of the picture again (a ghost at the screen's corner) once the car is about 195 units from the
  road's middle, so `race_pce.c LAT_LIMIT` (170, 50 beyond the kerb) holds it: `project()` moves the car back along the segment's normal
  and takes a sixteenth of its speed each step, and the pursuit (a straight road along x 4096) clamps `px`. The ground under the car is the
  map's own (sand 250 u/s), as before.
* Bank space: `boss_draw` and `hull` moved from $79 (now full) to $6d (`BOSS_DRAW` in `boss_pce.c`, `overlay_call(0x6d,boss_draw)`).

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

## Fifth session: full-size road again, with wrap copies (2026-10-05)

The half-scale road of the fourth session is gone: see docs/PCE_PLAYTEST_FIX_20261005B.md (kerb 4.5 dots a line, cars 1.17 x rows, four wrap copies of the lower rows in BAT rows 24-47
chosen per scanline by `road_stripes`/`irq.S`). Not run on hardware or in the emulator.
