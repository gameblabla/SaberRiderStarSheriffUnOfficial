# PCE WIP issues — user playtest, 2026-10-03

Baseline preserved in **`9d04f6e`**, committed as WIP before corrective work.
Implementation status below does not replace the user's visual/audio acceptance.

- [x] Title, options and hero selection are 320x224 screens modelled on the
      Saturn menus (see "Front end" in `PCE_PORT.md`). The briefing was cut.
- [x] Replace square-wave shooting with original sample `C66E1894` through
      resident PSG DDA: **5-bit PCM, approximately 6.99 kHz**. Sample IRQs restore
      MPR6; sample data survives stage reads. Impacts/special effects use PCM too.
- [x] Remove the synthesized jump tone; retain the selected character voice.
      Fireball's jump/hurt/death pack mappings now follow `src/audio.c`.
- [x] Stop timer/PSG channels before loads; wait for the death voice before
      respawn. Native sample captures finish with silent tails, including an
      interrupted shot. Campaign scenarios exercise death and game over.
- [x] Convert foreground occluders to hardware sprites with a common palette
      and separate cache IDs. SAT order keeps the HUD above scenery, and scenery
      above actors. Actor patterns are no longer cut by alpha masks.
- [x] Adapt dialogue to a compact bottom panel with original 2D speaker portraits.
      Preserve speaker/expression IDs and hero substitutions across pages.
- [x] Lock platform BYR at zero; remove the old text-HUD raster split. Wrap text
      writes within BAT rows instead of spilling across a scrolling row boundary.
- [x] Restore canonical up-right/down-right poses and diagonal run composites;
      use hardware flipping for the left-facing equivalents. Update facing before
      emitting a shot and retain diagonal horizontal velocity for down-right/left.
- [x] Remove both moons: level 1 uses a **blue** gradient; level 3 a **dark purple**
      gradient, per the user's correction. The old sky graphics are not baked.
- [x] Replace the text-only top-left HUD with original hero icons, hearts, panel,
      lives and power digits. Digits have higher SAT priority than their panel.
- [x] Disassemble `../PCE/Sound/adpcm_build_14_2bit.pce` and create a matching
      compressor: `tools/pce/adpcm2.py`. Native-ROM comparison validates 48 cases,
      954 samples, predictor saturation, step adaptation and partial-byte endings.

## Second pass (2026-10-04)

- [x] Platform playfields fill all 240 lines; the black top bar is gone (sky and
      scenery draw there, HUD sprites sit on top).
- [x] Platform stages render at 60 Hz: 120 loop iterations per 120 emulator
      frames in stage 1 (about 45% idle), measured with the cycle profiler
      (`tools/pce/profile.py`). Stages 3-5 also hold 60 Hz; stage 2 (race floor)
      runs at about 15 Hz and the Ramrod cockpit at about 29 Hz and were not
      reworked.
- [x] Title: Start / (Continue when a save exists) / Options, with a smooth
      dithered backdrop, the logo, and no text on black boxes.
- [x] Options: difficulty (hearts 3/2/1, enemy fire rate), lives, continues
      (capped per difficulty as on Saturn), music volume (off/low/mid/high) and
      exit. Continue screen after game over; credits after the ending.
- [x] Hero select: animated tunnel backdrop (palette cycling), full-colour
      selected portrait as hardware sprites, silhouettes for the others.
- [x] Fixed hero aim/motion sprite ID strides (heroes 1-3 showed the wrong pose).

## Third pass (2026-10-04, playtest feedback)

- [x] Death follows the main game on platform stages: the hero hops and falls
      where it died, then stands up at the last safe spot (about 1.2 s, lives -1,
      hearts refilled, 2 s of blinking invulnerability). No stage reload, no black
      screen. A hit that zeroes hearts while `lives` is 0 still ends in the
      continue / game-over screens. Falling into a pit and death zones kill too.
      Race, Ramrod and space still restart their phase.
- [x] April (and every hero) showed the wrong sprites while shooting/jumping:
      `presentation.py` captured the loop's `cell()` closure late, so the
      standing-shot and airborne poses were all cut from Colt's sheet. Fixed.
- [x] Jumping holds a single running frame (`JUMP_RUN_FRAME`) instead of the
      somersault cycle; the three airborne motion frames per hero are gone.
- [x] Dialogue looks like Saturn's: the real nine-slice tileset box (green,
      purple, red, blue per script tag) as two sprite halves behind the BG layer,
      the Saturn small font on BG cells, the avatar overlapping the box corner
      and a blinking arrow. See `story_pce.c`.

## Fourth pass (2026-10-04, playtest feedback)

- [x] Straight up / straight down aim poses for every hero (standing aim legs +
      vertical torso); down + direction keeps running and aims down-diagonally
      (crouch only when down is held alone). Aim poses also work in the air.
- [x] Slide pose per hero (cell 149). A slide knocks enemies down.
- [x] Enemies: 6-frame run cycles (walker, grunt), 6-frame death animations
      (walker, grunt, sniper) and their hit / death yells (original sfx 5 and 6,
      ADPCM events 3/4 in each hero bank, never cutting a hero voice).
- [x] Walkers keep their heading and turn round at walls and cars; they only
      fire when facing the hero.
- [x] The camera never scrolls backwards and the left screen edge is a wall.
- [x] Platform stages use the standard 224-line timing (was 240 lines).
- [x] Dialogue box: BG cells under it are blanked except the four corner blocks,
      which stay in front of the scenery, so no black margins show.
- [ ] "Field / foreground glitches": not reproduced in scans of stage 1 (static
      and scrolling); backward scrolling (now removed) was the suspect. Needs a
      screenshot if it persists.

## Fifth pass (2026-10-04, playtest feedback)

- [x] Hero animations use the original sheet cells: idle breathing (Saber 6 of
      his 12, April 6, Fireball/Colt their 2), the four-frame somersault for a
      real jump (walking off a ledge keeps the frozen run frame) and the death
      sequence (5 frames, April 8). They follow the motion rows in each platform
      scene (`pce_hero_pose` holds the offsets). `sprite_slot_of` grew to 480
      ids because stage 1 now uses 399.
- [x] Death: the hero no longer hops; the death animation plays where it fell,
      then it stands up at the last safe spot (80 ticks). Hits still only blink.
- [x] Continue screen text is centred on the panel (column 20, row 16).
- [x] Hero select: a portrait switch used to show half-copied patterns for two
      frames. The next portrait is copied to a second pattern buffer while the old
      one stays up; pieces and palettes then switch right after VBlank.
- [x] Stage 4 scrolling garbage: some 33-column windows held up to 966 distinct
      characters against the 928-tile cache, so the right column went unloaded.
      `tile_budget.py` redraws the cheapest single-use cells (648 of 24,960) with a
      matching neighbouring character, max window now 924.
- [x] Sprite glitches / foreground jitter: IRQ-masked VDC sequences and a scroll
      hold until the SAT is queued (see `pce-vdc-irq-hazards` notes).

## Sixth pass (2026-10-04, playtest feedback)

- [x] "Oh no! The Star Sheriffs" scene (level 1, x 349): the hero stops, the camera pans right 4 px a step to the
      outrider on the platform (focus x 730 from the level data), holds 1 s with the world running (the outrider
      freezes in alarm, then turns and runs off right), the text runs there, 2 s more of scene, then the camera
      pans back left to the hero. The world is frozen during the pans, as in the main game. Dialogue zones now carry
      focus x + the two hold times (`export.c` -> `flow_zones`, 14 bytes each); the state machine is `cut_step` in
      `combat_pce.c` (`cut_phase`). The outrider (type 28) used to be culled the moment it spawned (off screen) and
      walked left; it is exempt from the cull and runs right at 120 px/s.
- [x] Diagonal shots left a fixed point (18,-8) from the hero, up to 13 px off the gun. `pce_muzzle[hero][9]` holds
      the barrel tip of each pose (level, level running, crouch, up, down, diagonal up/down standing and running):
      measured from the pose art as the PCE draws it (`presentation.art_tips`; agrees with the source's barrel
      table for Saber/April to a pixel), straight down from the source game (`export_muzzle.c`). Diagonal shots
      fly 6 px per axis (the source's 0.7 x of 8), not 8. Select + down on the ground now draws the straight-down aim.
- [x] Enemies were sheepish: walkers/grunts moved 0.66 px a step (source: 120 px/s = 2 px a step, 1.2 x the hero) and
      stood still up to 75 steps after every shot. They now run at 2 px a step and a shooter plants its feet for 24
      steps only, then runs on.
- Code moved out of the full platform bank: trigger spawning is `encounter_pce.c` (combat bank `$70`).

## Seventh pass (2026-10-04, playtest feedback)

- [x] Galloping robot horses (type 11) were invisible (their sprite was one 16x16 cell of a multi-cell frame).
      The five gait frames are baked whole at 3/4 size (a full frame is 40 sprite pieces, the SAT holds 64):
      `horse0..4`, one gait frame shared by the herd. The source drops a column of 12 horses at once, 99 px apart,
      195 px behind the screen edge, running at the hero at 120 px/s; here five, 132 px apart. They trample the
      hero (jump over them) and kill every humanoid they touch; shots pass through. Spawn: `encounter_pce.c`.
- [x] Enemies stuck in cars: an edge spawn used to land inside a crashed car and jitter. `spawn_clear` (the source's
      face_and_probe) raises the spawn point 8 px at a time until a 64 px walk ahead is clear, so they drop onto the
      roof.
- [x] Bullets are the source's 8x8 orbs (blue hero / red Outrider) and the hero's muzzle flash (strip 8623249C,
      4 frames, diagonal or straight) is drawn at the barrel.
- [x] Opening cutscene: the blue Outrider (CRHC 0DB9F0E0: stand, "!" alarm, run) instead of the grunt, and the actors
      stay drawn while the text runs (`actors_draw` is shared by the world and the story overlay).
- [x] Shooting in the air keeps the somersault / run frame (aim poses only on the ground); shots leave the 14 px ring
      round the ball (the source's air muzzles).
- [x] Game over: the Nemesis painting fills the 320x224 screen (centre crop) and the lettering is added as light by
      OR-ing bit planes (`gameover_screen`): the cells under the letters hold the painting in the low two planes and
      the lettering in the high two, their four palettes carrying painting + painting-plus-light sets; the pulse
      rewrites those palettes. No sprites.

## Eighth pass (2026-10-04, playtest feedback)

- [x] Cutscene outrider flicker / dialogue box edges: the dialogue's cells and sprites now change in the same frame.
      Opening: sprites are queued, then right after the next VBlank the panel's cells are blanked column by column
      (`video_panel_blank`, increment-64 writes, done before the beam reaches the panel; no black band). Closing:
      the Arcade reads for the cells run first (`video_panel_restore_prepare`), the SAT without the box is queued,
      and after the VBlank the cells are written back (`..._apply`) - the old full background reload took several
      frames and left the in-front corner pieces up. The actors stay in the SAT throughout (no empty-SAT upload).
      The outrider's alarm lasts 75 steps so the "!" is still up when the text opens.
- [x] Background airships (types 24-27) removed from the PCE (they flew in far background layers): never spawned.
- [x] Foreground flicker: foreground sprite pieces are re-emitted every frame after the actors, so where more of them
      sit in a window than the SAT/scanline budget leaves, they come and go. `thin_foreground` removes 32x32 chunks,
      most crowded first, until every 288-px window holds at most 20 pieces and 8 per 16-line row (stage 1: 125
      chunks, stage 3: 103, stage 4: 29, stage 5: none) - a fixed removal, so what stays is steady. While the herd
      is on screen the foreground is left out altogether. Horses are drawn at 5/8 size (~15 pieces a frame), 150 px
      apart; with the foreground out the SAT stays at 30-52 of 64 and nothing is refused.

## Ninth pass (2026-10-04, playtest feedback)

- [x] Levels 1 and 3 have no foreground layer at all any more (even thinned it flickered). Stage 4 keeps its thinned
      one; `test_foreground.py` now checks foreground priority on stage 4 and asserts stages 1/3 carry none.
      The horse herd stays; with the SAT free of foreground it holds 33-52 of 64 entries, nothing refused.

## Tenth pass (2026-10-04, playtest feedback)

- [x] Enemy hit points follow the source: every humanoid dies to one hit (they had 2); a shield sniper takes 4/6/8
      shots by difficulty (was 6). Stage 4's arena grunts too.
- [x] Enemies popping in and out: an optional actor the SAT could not take for a frame used to be deleted. A refused
      draw is now just skipped, and at most three humanoids are alive at once (a new one waits for a free place), which
      is what the SAT holds beside the HUD and hero. Enemy shots: at most 4 at a time.
- [x] A walker or grunt dropping from a platform shows its two fall cells (source anim 0x30) instead of a frozen run
      frame (`walker_fall0/1`, `grunt_fall0/1` after the sniper's death cells; drawn while `coll&4` is clear).

Remaining integration/acceptance work:

- [ ] Integrate the reference ROM's two-channel, scanline-delivered 2-bit ADPCM
      playback with the race raster scheduler, and measure its CPU use in-game.
      The reported 17% budget is not claimed. The current disc uses the requested
      5-bit PCM fallback. See `PCE_AUDIO_REFERENCE.md`.
- [ ] User playtest the rebuilt disc, especially sound/music balance, dense
      foreground scenes and dialogue appearance. Optional scenery/enemies can
      be dropped when the real hardware sprite/cache limits are exceeded.
- [ ] Continue broader source parity and physical hardware acceptance work
      already documented in `PCE_PORT.md`; current tests are seeded scenarios.

Validation uses the supplied accurate Mednafen PCE core and Super CD v3 BIOS.
Reports: `build/pce/{verification,campaign-verification,foreground-verification,
presentation-verification,audio-verification,adpcm2-verification}.json`.
Screenshots: `build/pce/presentation-review/` and campaign captures.
Actual audio captures: `build/pce/audio-review/*.wav`.
