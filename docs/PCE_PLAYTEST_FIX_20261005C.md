# PCE playtest fixes, 2026-10-05 (third batch)

Checked in the accurate emulator (screenshots, VRAM and SAT dumps, audio capture); `tools/pce/test_dialog_restore.py`, `test_priority.py` and `test_presentation.py` pass.
`test_port.py`, `test_campaign.py` and `test_audio.py` still stop on things that predate this batch (the diagnostic pages and the DDA shot sample they drive are gone).

## Palette RAM is only written in the vertical blank (or with the display off)
* `pce_config.h`/`vce_pce.c` ($75)/`irq.S`: every `pce_vce_*` call is now `vce_copy`, `vce_set` or `vce_read`. With the display on, ONE palette is queued (a ring of four,
  written by the VBlank interrupt at the moment the new SAT comes up: the sprite cache's palettes, a dialogue's colours); more than one palette, a single colour or a read-back
  waits for the vertical blank first (fades, flashes); `vce_copy_now` is for code that has just returned from `video_wait` (the dialogue panel's cells and colours go together).
  With the display off everything is immediate. `video_display` keeps `pce_display_on`.
* Per-frame writers were cut down: the space stage's hull palette is written only when a hit changes it, the screen flash once.
* Verified with an instrumented Mednafen (`VCE` accesses logged while the display is on and the beam is in the picture): zero in the title, hero select, stage cards, stages 1, 2, 5, 6,
  7, the boss fights, the victory painting and game over (the same probe counts 7561 for a deliberate violation).

## Transitions
* A life lost with lives left respawns in place on the platform stages (no fade); only the game over fades. Ramrod's arena respawns in place too (the wave begins again, `m6_c.c dead`).
* The stage card now fades to black before the stage's own screen comes up from black (`loader_scene`).
* CD-DA starts no longer hold the game up: the two drive commands the BIOS sends (NEC D8 cue, D9 end/mode) are sent by hand on the SCSI bus (`loader_pce.c`); the drive answers
  after its seek (about 40 frames, formerly a frozen screen, twice), `audio_tick` polls and sends the second. The BIOS calls remain as the fallback when the bus is busy.

## Dialogue
* Race: the box was drawn at BAT row 20+ (the text went to rows beyond the BAT, so a blank green box): `story_pce.c` row 5 again for the race. The box's sky cells and the road's wrap
  copies are written back from Arcade RAM when the dialogue closes (formerly the whole sky was reloaded, wiping in over 20 frames); while the box is up the frozen road uses no wrap copy.
* Stage 6's dialogue restores the HUD palette (31) it shares with the dialogue corners; the arena opens facing the planet.

## Sounds
* The PSG tone channels never played: the waveform was written with DDA set, which the chip ignores (`psg_init`). Fixed, and the boss guns and the shot are rebuilt from the
  recordings' own body (pitch and loudness of the strongest partial below 1.3 kHz on the tone channel, the rest on the noise channel with a clock from its brightness: `build_audio.py`).

## Ramrod's arena (stage 6)
* Waves thinned (3 greens, two at a time; a red with two greens, then a pair; the Commander with one green), armour worth twice as much, the guns heat 9 a shot and cool 3 a step when idle.
* Radar in the top right (sprites, 32x32, one pixel 64 units): dots green / red / gold, white when about to fire, pink for plasma.
* Everything is 1.6 x bigger (sizes and rows under the horizon): the nearest mech is drawn at four more steps (to 1.16) as 16 pieces of 32x32, written into one of two alternating
  pattern buffers ($6800, $3000), so a pose change never tears; palette 30. The arm (ten frames, 32x16 pairs) likewise in two buffers ($2800, $4000), palette 29.
  The sprite table is budgeted: the fixed HUD is not in the sprite cache (one 16x16 pattern per piece in $7800), messages are admitted last and moved to the front, boulders go last.
* The floor is a flat ground texture warped by the horizontal-blank interrupt (`irq.S .Larena`, 24 groups of four scanlines): BYR gives each group the texture line its depth asks
  for plus the way walked, BXR the strafing by depth band. The tables live in console RAM (`arena_floor`): the staging banks (MPR6) are remapped by the Arcade block copies,
  and an interrupt that read from there corrupted every sprite upload.
* A hit no longer blinks the mech; lock bar instead of brackets; one reticle piece.

## Code space
* `ui_fade_*` moved to $7b; the CD music code and the PSG are in $75; the resident bank has about 220 bytes left, console RAM about 85.

## Follow-up (structural, not run)
* Arena dialogue: `story_start` zeroes the scroll's vertical shake for stage 6 too (the BG panel sat a few lines off its sprite corners after a killing blow's shake), and `m6_frame` forgets the arm's pattern buffers after the draw (a punch still in flight in the frame that raised the dialogue had reloaded them over the dialogue's characters at $4000).
* Ramrod's bolts live exactly as many steps as it takes them to reach their target (they used to carry on across the middle to the other side).
* The Renegades keep a range of 800-1300 units (formerly 360-720), charge far less often, and shoot from there.
