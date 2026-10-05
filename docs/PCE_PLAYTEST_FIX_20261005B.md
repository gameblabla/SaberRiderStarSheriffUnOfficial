# PCE playtest fixes, 2026-10-05 (second batch)

Structural changes only: the build links and the disc image is written (`make -f Makefile.pce all`); none of it was run or tested in an emulator.

## Level 2 (the race)
* **Road at full size again, off-road kept.** The picture's slopes are back to the source's widths (kerb 4.5 dots a line, `build_assets.py road_assets`; the world-to-dots scale
  `road_pce.c dots_q4` 77/512, `race_draw_pce.c project_point` 154/256) and the cars are 1.17 x the rows below the horizon wide again (they had shrunk to 0.59).
  The 1024-dot picture cannot hold a road this wide with sand either side, so the lower rows (image rows 8-13) have four more BAT copies (rows 24-47: right-wrap and
  left-wrap, both stripe phases) in which the half of the picture that would wrap into the window is sand. `road_fill.S road_stripes` chooses the copy from the sign and
  size of a scanline's BXR high byte (class 0/1 normal, +2 right-wrap, +4 left-wrap), `irq.S` turns the class into a 16-bit BYR (`pce_byr_lo/hi`). Upper rows clamp
  the offset to +-480 dots (`dots_q4`), where the wrapped part is still sand. The car may leave the road to `LAT_LIMIT` (170 units) as before.
  The wrap copies share BAT rows 24-47 with the race dialogue's glyph patterns (`race_box`): while a dialogue is open a wrapped scanline would show glyph data;
  `video_race_sky` puts the road's cells back when it closes.
* **Firing no longer slows the loop**: a shot is projected with the Q7 camera and a coarse offset (four products instead of ten) and without the second division.
* **Cars explode** (five frames, big and small, baked from `assets/mode7.png` explosion, `PCE_CAR_EXPL`) with the burst's sound sample (the source's sfx 14, now the PCM
  `impact`). Wrecked rivals, mines, escorts and the leader all burst (`race_foes_pce.c blast`, `race_pce.h Blast`).
* **Every rival still running has finished three laps before the car: the race is lost** (a life, and the race again; game over with none left) (`race_field_pce.c field_update_call`).

## Sounds and transitions
* The "ugh" (the PC's sfx 4, `EB3309DC`: the hero's death) now only plays as Fireball's death voice (and sfx 3 as his hurt); the PCM `impact` is the grenade's burst. The menu
  blip is the PC menus' tick (sfx 0). Dialogue pages and the stage card's letters tick as on the PC.
* Common fades (`ui_pce.h ui_fade_out / ui_black / ui_fade_in`): the title fades to black before the options or hero select, which come up from black; every stage
  (`main_pce.c change_stage`) comes up from black. The victory painting leaves after 5 s (11 s with the jingle) with a fade.

## Platform stages
* A fall draws the frozen run frame, so the shot leaves the run pose's barrel (not the somersault's ring) (`play_pce.c`).
* Spawn points below -999 (level 1's respawn areas) throw the enemy up out of them (167 px/s) and hold it until it lands (CF_SPAWN_FALL, anim 0x34: cells 9-10, new
  sprites enemy_base + 76..79); type 5 is the brown grunt with its own run/fall cells (enemy_base + 68..75).
* A grenade bursts (five 32x32 cells, enemy_base + 80..84; `Shot.enemy == 4`) with the burst's sound on the ground or on the hero.
* The last convoy of level 1 has three horses fewer; placed enemies whose zone the hero crosses during a stampede are gone (they would have been trampled), so none walks in afterwards.
* Hyperjumper (stages 3, 4): its sprites are moved behind every bullet (SAT order), and it has the PC's poses: engines lit while it moves, the gun's two firing frames, the front
  pose firing. One pattern set serves the four side poses (23 cells), so a pose change only swaps a piece list (`build_assets.py hull_set`, records 4-7, `boss_pce.c hull_pose`).

## Stage 5: Dark April (`dark_pce.c`)
Rewritten after `darkapril.c`: the wait, the radio call, forming out of the motes, the meeting, a beat in which the hero is healed, then the fight (she keeps her range, closes in
firing, aims at his height, hops his fire), death and dissolve, the ending. April's own cells as a violet shadow (`presentation.py`, dark + 0..17), her physics are the hero's.
Not done: the mirror of the hero's buttons, her slide and leap, the afterimages and motes.

## Stage 7: the cruiser (`space_pce.c boss_step`, `scenery_pce.c`)
WARNING (the music stops), the nebula fades out, the cruiser flies in from the right (BG scroll, 5 s), its greeting (the PC's SCRIPT_BOSS, story 1) is shown with the hull scrolled to a
multiple of 8 so the box lands below it, then the fight: it bobs, its gun ports (at the PC's port positions) fire aimed shots one after another, drone swarms leave the bay, mines
from the second stage, and the nose cannon gathers (sparks as sprites), then fires a beam of BG cells (palette 4, the last five tiles of the hull set), 22 dots in the last stage.

## Code space
The banks are full: new code went where the room is (`overlay_call` bank numbers are in each file's comments), `minsize` is set on the cold banks ($6e flow, $70, $71, $73, $78) and
the diagnostic page tests of stages 6 and 7 and the effect demo (`effect_pce.c`) are gone. `muls` is one resident copy (`race_pce.c`).
