# PC Engine: boss sounds and passes, victory, race sky/floor and stage intros — 2026-10-04 (second revision)

Follows `PCE_BOSS_RACE_PRESENTATION_FIX_20261004.md`. Source of the requests:
`../codex-session-01a10792-cc77-7792-90f3-21376dbc42e4.md` and the two supplied states
(`saber_rider.a63952863ebbf02ec5acf27b96d026f2.mc0`, `…-race.mc0`).

## Platform stages

- **Security camera.** Its 16x16 cell carried a patch of the building wall behind the bracket, in a different grey
  from the background tiles it sat on (visible as a coloured block, and moving against the wall under rumble). The wall
  greys connected to the cell's edges are now transparent (`tools/pce/build_assets.py` `key_wall`, both animation cells).
- **Dialogue.** The blinking arrow at the corner of the box (the "text cursor") is gone (`story_pce.c`).
- **First stampede.** 8 horses instead of 12 (`encounter_pce.c`); spacing and speed are unchanged. The other two convoys
  keep their length.
- **Bosses (levels 1/5 gunship, 3/4 Hyperjumper).**
  - The ship was drawn at full size on its far and mid passes. As in the source (a ship flying in from the distance), it
    is now a half-size hull on the far pass, a three-quarter hull on the mid pass and the full hull only for the fight
    (three records per stage in `boss_big`, reloaded into the same pattern pages while it is off screen; `boss_pce.c`
    `hull_level`).
  - Sounds from the source: the engine pass (`sfx 0x13`, at the start and at every later pass / entrance), the gun
    (`0x10`), the rider's gun (`0x12`, and both on the step the gunship fires both), every blast of the burning wreck
    (`0x11`) and the big bang at step 8 (`0x15`). They are CD ADPCM events (tones 12-17) in every hero's voice bank at
    4 kHz (engine, bang) and 5.33 kHz (guns, blast) so that the largest bank, Saber's, is 62 KiB of the 64 KiB; the new
    `rate` column of `voice_groups` carries the BIOS rate. Hero voices (hurt/death/dialogue) are never cut by the guns.
  - Code moved between banks to make room: the boss simulation stays in `$7b`, `hull`/`boss_draw` moved to `$79`.

## Victory

- The jingle (CD-DA track 6) played at `result==1` was cut at once by the UI disc read. It now plays once over the
  held last frame, the picture whitens (`ui_lift`, bank `$69`), and only then is the painting read.
- The painting is the centred 320x224 of the original 426x240 (it used to be squeezed). MISSION / ACCOMPLISHED is drawn
  like GAME OVER: the lettering is added as light by the bit-plane OR trick and pulses (`frontend.py` `glow_screen`,
  shared with the game-over screen; the painting under the lettering is darkened to 80%). The victory music (track 7)
  plays once on it.
- The 19 paintings are one small extent each of a new `victory.bin` (about 38 KB), not part of `ui.bin`: a stage's end no
  longer reads the whole 860 KB front-end archive (`ui.bin` is now 178 KB). `victory_pce.c` (bank `$72`).

## Race (stage 2, both phases)

- **Sky.** A blue gradient of plain background tiles (13 shades walked along the 9-bit lattice + the haze, 8x8 ordered dither, 16 distinct characters;
  `race_sky`), down to the horizon (scanline 113), then the haze of the ground out to scanline 120. The old picture is
  not baked.
- **Floor.** The raster "framebuffer" begins at scanline 120 instead of 128 and has 19 strips instead of 24: the seven
  far ones are 8 scanlines tall and sampled at half resolution, the twelve near ones 4. The first strip is the ground
  from 916 units out, so the road runs to the horizon haze. `floor_tables.py` (19 distance/stride rows), `irq.S` (two RCR
  step sizes), `floor_pce.c`. A pass is 1,984 samples instead of 2,304.
- **Cars.** A car projected beyond 900 units would have stood above the floor's first scanline, i.e. in the sky: those
  are not drawn, so every visible car is on the road (`race_draw_pce.c`).
- **Performance.** A BAT row is block-copied (the samplers write the low bytes of VDC words, so no per-entry store loop
  remains), the per-row 32-bit multiply of the geometry address and the HUD's 32-bit arithmetic became 16-bit.
  `tools/pce/profile_race.py` (seeded five seconds, the same scenario before and after): with the seven rivals **61 -> 75
  road updates per 300 video frames (12.2 -> 15.0 per second, +23%)**, without them 74 -> 89 (14.8 -> 17.8). The race is
  still not 60 Hz; the floor sampling is 46% of all cycles.
- **Intro.** Every stage opens with its title card (stage number, name and sub-title, `card_pce.c`, as `game.c title_draw`:
  the card stays up through the disc read, replacing NOW LOADING), and the race's opening ends with the controls card
  (STEER / ACCELERATE / FIRE / TURBO / BRAKE / THREE LAPS!, `story.py` `RACE_CARD`) as two extra dialogue pages after the
  seven lines of the PC's opening.

## Verification

`make -f Makefile.pce test` passes in full on the final disc (native-disc regression, campaign, boss, herd 60 Hz with held fire,
foreground, presentation, dialogue restore, audio and codec checks), plus the new `tools/pce/test_boss_passes.py` (hull
sizes by pass 2 -> 1 -> 0, the boss events in every hero's ADPCM bank, ADPCM audible during the fight), and screenshots of
each item from the rebuilt disc.
Physical hardware has not been measured; a state saved with the old disc restores the old program with it, so start the
rebuilt `build/pce/saber_rider.cue` or an in-game checkpoint.
