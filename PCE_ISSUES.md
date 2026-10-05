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

## Eleventh pass (2026-10-04, playtest feedback)

- [x] The horse herd is a cutscene at the source's 1:1 size. A frame (128x80) is baked as VDC big sprite cells - four
      columns of a 32x64 and a 32x16 sprite - so a horse is 8 SAT entries instead of 33 pieces (`horse_frames` in
      build_assets, `herd_pce.c`). The 40 patterns of the current gallop frame are streamed into 10 reserved pages of the
      sprite cache (two buffers, so the frame on screen is never overwritten). Five horses 192 px apart gallop at
      120 px/s; at most two are on screen and the SAT stays at 22-38 of 64.
- [x] While the herd runs the camera is locked (the hero is held inside the screen, nothing else spawns) and it is
      released when the last horse has gone; the camera then catches up at most 4 px a step. All three convoys of
      level 1 (x 2252, 5916, 8648). Horses trample humanoids and hurt the hero; shots pass through.
- [x] Opening cutscene: the outrider's hit point change (3e1009e) had made it spawn "alarmed", so the "!" showed at
      once and it was already running when the text opened. Fixed, and it now exists from the start of the scene:
      idle while the camera pans, "!" when the camera arrives, the "!" held through the text, runs off after it -
      the sequence traced from the PC game (`headless` run of the same level data).

## Twelfth pass (2026-10-04, playtest feedback)

- [x] The hero (and every actor and the boss) vanished while a dialogue box was open: the story overlay emptied the
      SAT and drew only the avatar and box. Platform stages now keep them in the SAT (`hero_sprite` = the id
      `play_draw` last chose) and the box sits at the top of the screen like the race stage's, because a bottom box
      (y 144-208) covers exactly where the hero stands. The box cells, arrow and the restore after the text follow
      `story_y`.
- [x] More PC voices (all from the PC game's own sample table, `tools/pce/build_audio.py`, CD ADPCM banks of
      37-43 KB per hero; the variant is picked at random as in `src/audio.c`):
      hero hurt x3 and death x2 (Saber/April/Colt: the generated grunts; Fireball: the sfx 5 / sfx 6 yells), a fall voice
      (pit), enemy hit x3 (sfx 5 yells), enemy death x3 (a hit yell plus a death yell 3 frames later, mixed at build
      time as the PC layers them), the cutscene Outrider's "!" alarm (sfx 22, `E105C92A`) and the level-1 dialogue
      voice line `FDB525F9` ("Oh no! The Star Sheriffs!!!"), which the dialogue script names on its first line and
      which plays when the box opens (`voice` field of the 16-byte flow-zone record; `export.c` writes `sfx`).
      `audio_effect` tones: 7/8 enemy hit/death, 9 alarm, 10 dialogue line, 11 fall; the bank logic moved into
      audio bank `$75` (`voice_pick`), the BIOS call stays resident.
- [x] `test_audio.py` plays every new event on all four heroes and checks all variants are reachable;
      `test_campaign.py` checks the hero/actors stay in the SAT during dialogue and the voice line is playing.
- Known, not fixed: when the horse herd tramples the hero on a frame where the horses fill a scanline, one
      essential sprite can be refused (`essential_overflow` +1, one frame). It depends on the gait phase (reproduces on
      the previous commit at other timings); `test_herd.py` now keeps the hero invulnerable on the run to the lock.

## Thirteenth pass (2026-10-04, playtest feedback)

- [x] Aiming down-right while running sometimes drew the straight-up pose: the running down-diagonal strip was indexed one cell
      too far (frame 5 landed on the straight-up cell).
- [x] BG palette glitch (white pixels from y 192): the renderer forces BG colour 255 (palette 15 index 15) to white for the text
      font, but the bottom band's last palette used it as an ordinary colour. The baker now keeps index 15 of palette 15 unused
      (`build_assets.native_background`, `tile_budget`).
- [x] Horse herd: the convoy is the source's 12-13 horses 99 px apart (was 5, 192 apart), instantiated lazily as the column nears
      (`herd_feed`: the actor pool holds eight). The herd is drawn right after the hero, before shots and enemies, so it is what
      the SAT / scanline budget gives way to (muzzle flash and shots are optional while it runs). Horses are y-aligned to the 8-line
      band so the batched emitter takes them; 60 Hz still holds in all three herds (`profile_gameplay.py --require-60`).
- [x] Herd trample bug (one essential sprite refused for a frame): a hit changes the HUD heart icon (14 pieces = 4 contiguous
      pages) and no such block was free beside the herd's 20 reserved pages; the retained firing poses now give theirs up when the
      HUD rebuilds. `test_herd.py` no longer needs an invulnerable hero for this.
- [x] Enemies follow the source (enemies.c, whole 1/60 s steps, `enemy_pce.c` + `shots_pce.c`): grunts fire ONE shot per life, on the 16th
      step of a 24-step stand, with the hero ahead of them (4% a step); snipers aim in eight directions at the hero (settle 13 steps,
      then 1% a step), with the sniper body's own aim cells; kneelers (types 8/9, previously drawn as snipers) crouch and lob grenades
      on the source's parabola (7% a step, 24 steps of wind-up); shield snipers keep their pace. Shots move in Q8 px/step (166 /
      200 px/s, 0.7 x on diagonals), hit boxes are the source's. Actors are 21 bytes now (`aim`, `mode`), shots 13 (`NSHOTS` 16).

- [x] Bosses follow the source timelines (`boss_pce.c`, new code bank `$7b`): the level-1 gunship (stages 1 and 5) makes its far-layer and mid-layer
      passes (6.5 px a step), then sweeps - flies in at 2.3 px a step to a hold at the screen edge, holds 80 steps with its diagonal
      lasers, leaves on the far side and returns from the other (the only phase it takes hits in), its rider covering the other side
      (silent for steps 160-184 of a round), no contact damage; the Hyperjumper (stages 3 and 4) runs the source's far/mid pass, side
      in-hold-out, low ground pass (jump it or slide), drop-in fire-down and exit cycle with its hull hurting on contact. Both burn
      out for 228 steps (the wreck falls) before the stage clears. The gunship is now the boss character's own whole frame 0 at 5/8
      size (the old art was a stretched 16x16 cell) and the Hyperjumper keeps its aspect (3/4 size). Code space: the CD buffer is
      40 KiB ($76-$7a, was 56 KiB) so banks $7b/$7c are spare code banks (`PCE_BOSS`, `link.ld`, `check_elf.py`).

## Fourteenth pass (2026-10-04, playtest feedback)

- [x] Level 2 floor at a much higher internal resolution (`PCE/Framebuffer/Wolf3D.txt`): the floor is BAT pair characters at the
      512-dot clock, 24 BAT rows of four lines, 128 samples across, each row its own perspective row (geometry for 128 headings
      in the Arcade RAM, `floor_tables.py`), time-sliced: `floor_update` samples a budget of pairs a call and the budget follows the
      loop (it keeps passes to two or three frames; the pass no longer waits out the rest of a frame, see `main_pce.c`). A full
      floor commits about 7-8 times a second, the sprites are projected from the camera of the floor last completed
      (`floor_shown_*`) so cars and road agree, and the car, HUD and shots run on top at the loop's 20 Hz. No black bars: the sky
      is the 512-wide art and the HUD is sprites (`hudart.py`), the dialogue box restores the sky cells afterwards.
- [x] Level 2 plays like the source (`race_*_pce.c`, banks `$79` core, `$7a` shots/leader/escort, `$6d` the field, `$7c` sprites and
      HUD): free steering car with the ground deciding top speed (road / kerb / sand, turbo), seven rivals on the circuit's rails with
      the source's speeds and rubber band, Black Hornet mines and shots, three laps, a top-three finish needed (a life and the race
      again otherwise), then the pursuit of the leader (escorts, mines, his booster, the rear gunner, rams) and the boss fight.
      The race ticks at 60 Hz; the field every other tick with doubled steps. Everything is 16-bit (no library division in the hot paths).
- [x] Level 6 phase 1 and phase 2 HUDs follow the source (`hud_pce.c`, bank `$7c`): the cockpit is cropped from the 426-wide PC
      cockpit, the mech frames are baked at 8 widths and drawn with the slice technique (`PCE/scalingsprites`), the HUD is sprite
      pieces rendered from the source's fonts (arm/gun bars, wave, radar, heat; shield cells, power pips, torpedoes, hero power,
      the cruiser's name and hull bar). The HUD sprites of a stage share one palette (`hudp_` in `build_assets.py`) and live in the
      shared-palette slots of the sprite cache (`pce_hud_base`), so a HUD cannot starve the world of cache slots (the cruiser fight
      used to drop its own sprites).
- [x] Herd, aim, palette, enemies, bosses: see the thirteenth pass. The aim-up-while-running-down-right glitch and the white row
      at BG Y=199 are fixed there.
- [ ] Not reproduced: the black screen after "select a hero, die once, start a new game, select another hero". Tried the
      continue timeout, game over, title and every hero pair, and the hero change from the run menu after dying; all showed the
      stage. If it still happens, the exact sequence (and whether a CONTINUE was used) would help.
- [x] The game-over picture: resolved in the fifteenth pass using the local `go_320.png`.
- [x] The race's half-width dialogue: resolved in the fifteenth pass.
- [x] Stage loads read 12-sector chunks now (the CD buffer is 24 KiB: `$76-$78`; banks `$79-$7c` are code).

## Fifteenth pass (2026-10-04, remaining issues from the session log)

- [x] The supplied Nemesis picture was available locally as `go_320.png`; it is now tracked at
      `assets/pce/gameover.png` and used by the PCE baker. The original GAME OVER lettering still pulses through
      palette changes, with its fade and one-shot jingle. The asset is a Makefile dependency.
- [x] A held confirmation could cross the GAME OVER fade into the title: `frontend_start` reset `previous` to zero,
      so the same press selected START again. It now samples the held buttons before loading the title and requires
      a fresh press. The regression exercises lethal damage, the death animation, GAME OVER, the title, a different
      hero and responsive gameplay for all four heroes; it also checks the lettering changes while the painting stays still.
      This fixes a demonstrated input leak; the original report's persistent black screen has not been reproduced.
- [x] Race dialogue now spans 448 of the 512 dots, with horizontally doubled source glyphs and portraits. The box
      uses background characters rather than exhausting the SAT. Its 32-character reservation is at VRAM word
      `$4000`, below the normal font; the doubled font occupies unused BAT rows 24-47 (`$0c00-$17ff`). Sky characters
      are limited to 256 below `$4000`; the measured sky needs 48. Palette 14 is reserved for the dialogue, with the
      white glyph ink at index 15; sky palette 0 and floor palette 15 stay independent. Closing the dialogue restores
      the sky. `test_campaign.py` checks the actual 56-character box width.
- [x] The race briefing used to freeze the initialization checkerboard before a complete floor was ready. Stage
      initialization now completes and presents one floor with zero simulation steps before opening the briefing.

Validation: `make -f Makefile.pce test` passed in full (native port, campaign, herd, herd rendering, all three
60 FPS profiles, foreground, presentation, audio and both ADPCM checks), plus ELF bank checks. Restart cases use
normal difficulty, default lives and full music volume, holding confirmation through the fade and checking that
the title waits for a fresh press. Emulator screenshots are
at `build/pce/{race-dialog-review.png,presentation-review/gameover-*.png,presentation-review/restart-*.png}`.
The floor's existing redraw cadence and physical hardware acceptance remain as documented above.

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

## Dialogue restoration and sprite budgets (2026-10-04)

- Platform dialogue panels use BG tiles and opaque-backed font glyphs. Only
  four rounded corners remain sprites: 4 SAT entries instead of 42, and at
  most 2 scanline width units instead of 14. Portraits and actors stay visible.
- Closing restores the original panel's world columns, font and reserved BG
  palette from preloaded Arcade RAM. Sprite cache pins survive the closing
  DMA; retained foreground is re-admitted without overwriting live corners.
- Sprites start each 16-line strip at its first opaque pixel, retaining every
  indexed pixel and the original anchor in both facings. Exact scanline counts
  replace conservative platform bands; retained foreground records and reset
  paths now use the same representation. Hardware sprite limits remain on.
- `make -f Makefile.pce test-dialog` checks panel paging and complete BAT/font/
  palette restoration at three scroll phases, including a camera catch-up
  across a column, plus pause/resume without stale menu text. Review images: `build/pce/dialog-review/`.
- Accurate-core checks passed: native-disc regression, campaign scenarios,
  menus/all sixteen aim poses, foreground priority, 160 herd SAT cases and
  32 HUD cases. Each convoy still presents 300 new frames in 300 VBlanks;
  continuous firing after the three convoys peaks at 9/7/7 width units and
  both rightmost background columns match the resident cache.

## Fifteenth pass (2026-10-04, playtest of the previous revision)

- [x] Security camera cell no longer carries a differently coloured patch of wall; dialogue has no blinking arrow; the first
      stampede has 8 horses.
- [x] Gunship / Hyperjumper: half-size hull on the far pass, three-quarter on the mid pass, full size only for the fight; the
      source's engine, gun, rider's gun, blast and big-bang samples play (CD ADPCM events in every hero bank).
- [x] Victory: the stage jingle (CD-DA track 6) plays instead of being cut by the disc read, the painting is the centred 320x224
      of the original with MISSION ACCOMPLISHED added as pulsing light like GAME OVER, and track 7 plays on it.
- [x] Race: blue gradient sky of plain tiles, floor from scanline 120 in 19 strips, no car drawn above the floor, road updates
      12.2 -> 15.0 per second with the seven rivals; every stage opens with its title card and the race ends its opening
      with the controls card.
- [ ] Not done: the road still updates at about 15 Hz, not 60 (floor sampling is 46% of the cycles); physical hardware
      acceptance of the new ADPCM rates (4 and 5.33 kHz) and of the boss sound balance.

## Sixteenth pass (2026-10-04, playtest of the previous revision; details in `docs/PCE_PLAYTEST_FIX_20261004C.md`)

- [x] MISSION ACCOMPLISHED is opaque sprites over the painting (no translucency); GAME OVER keeps its pulsing light.
- [x] NOW LOADING returns under the stage title card's text.
- [x] Stage 1: the hole beside the security camera is closed (the wall is in the background); background seams every 256 dots
      from parallax layers (buildings, a doubled van nose) are gone (continuous layers with steps in empty places).
- [x] Dialogue boxes have one flat inside colour; the speaker portrait palette is reloaded each page (April after Claudia).
- [x] Race: no speedometer; the mine spin-out turns the car (11 baked poses, two sprite objects each).
- [x] Stage 3: the hero walks in from off screen to the radio scene; night colours on every background layer.
- [x] Stages 3 and 4: Hyperjumper's front pose is a hull record in Arcade RAM swapped in while the ship is off screen; stage 4 has
      no foreground layer.

## Seventeenth pass (2026-10-04, playtest of the previous revision; details in `docs/PCE_PLAYTEST_FIX_20261004D.md`)

- [x] Stage 7: enemy ships drawn unflipped (they faced backwards); they explode (fireball frames) with the enemy's yell + explosion
      (CD ADPCM `enemy_death`), mines burst with the PCM impact, and the cruiser breaks up over 72 steps with the big bang before the
      closing dialogue.
- [x] Stage 7 HUD: one narrow column at the top left (one sprite per scanline at most), vertical cruiser hull gauge at the right edge.
- [x] Stage 7 nebula scrolls its whole 512 dots and wraps without a one-frame reload.
- [x] Background streaming: a tile slot released by a column that scrolled out is held until the next call (the old picture is still
      displayed until the next VBlank). Not reproduced with the available checks, see the doc.
- [x] Race: the steering poses now show (the lean was an integer division that stayed 0) and the car slides with its lean.
- [ ] Three regression tests fail on the previous pass's uncommitted work (checked: they pass on the last commit, and still fail with this
      pass's changes removed): `test_herd_render.py` (occupancy assertion, exact=0 band representation, a mode `video_scene` no longer
      enters), `test_herd_visibility.py` (the injected `play_draw` call never returns) and `test_foreground.py` (stage 1 has no foreground
      entries any more, so it has nothing to check). Not investigated further; all other PCE tests pass.

## Eighteenth pass (remaining WIP and supplied captures; details in `docs/PCE_WIP_COMPLETION_20261004.md`)

- [x] Mountains keep intact native source strips, removing the sector cuts shown in the supplied save.
- [x] Security-camera wall repair handles transparent sky, closing the missing wall tile beside the camera during stampedes.
- [x] Dialogue corners use reserved patterns and four SAT entries; closing restores scenery and the shared sprite palette at VBlank.
- [x] Stage 7 cruiser uses 180x98 background art, black surroundings after a palette fade, scroll-register movement, larger collision
      outlines and source track 11. Bombs and powers flash the screen white with grunt/explosion sounds. No fight-time disc reads.
- [x] Race sky has two scrolling cloud/horizon bands with a repeated BAT; road raster strips remain at scanline 120.
- [x] Previous WIP retained: outrider bottom panel, stages 3–5 entrances, drop-through recovery, falling diagonal aim, solid projectile
      collisions, gliding boss camera, scenery behind actors, pursuit music/steering/shots, gentler leader and turbo/HUD fixes.
- [x] Native test trampolines reserve `$3bf0–$3bff`, avoiding application BSS. Foreground checks handle stage 5's empty source layer.
      The three formerly failing regression checks now pass. Campaign verifies a drop followed by a jump back onto the platform.
- [x] All three stampedes present 300 new frames in 300 VBlanks while firing, with zero essential overflows. Equal horse upload
      slices remove the third stampede's firing-start slowdown.
- [x] All 17 regression checks pass on the rebuilt disc, including campaign, menus/restarts, dialogue restoration and native audio.
      The first suite invocation stopped at the obsolete foreground fixture; presentation/audio targets passed the remaining checks
      after its correction. See the completion note for reports and run logs.

## Nineteenth pass (playtest of the previous revision; details in `docs/PCE_PRIORITY_BOSSES_ENEMIES_20261005.md`)

- [x] Platform sprite priority is tracked: hero, herd, boss, enemies, enemy bullets / grenades, the hero's bullets, muzzle flash,
      foreground. The hero's bullets are dropped first, then the enemies' bullets, enemies last; refused enemies pause the filler
      spawns and refused enemy bullets pause enemy fire (`priority_pce.c`).
- [x] Placed enemies (snipers, kneelers, shield snipers) spawn as in the source: the three-enemy cap no longer skips them and they are
      no longer culled the moment they spawn a screen ahead of the hero. `encounters` moved to bank `$78`.
- [x] Kneelers' grenades: the source sprite in eight pre-rotated poses, swapped as it flies.
- [x] Shield sniper (stages 4/5): shield up / burning / bare and both deaths from `assets/forest/sniper.png`; the shield absorbs front shots.
- [x] Level 1/5 gunship has its rider (a second gun covering the other side, lowering to 45 degrees) baked into the hull cells; the hull
      sits at the source's height.
- [x] Level 3/4 Hyperjumper front pose: symmetric, five cells wide, right half drawn by flipping the left half's patterns (the old
      six-cell hull lost pieces to the HUD's scanline units).
- [x] Level 4: flat-band blue sky (no black holes, far fewer unique characters), 32 characters of cache slack against the scrolling
      edge glitch, cabin walls in front of the hero again.
- [x] Continue screen: the PC game's tick (sfx 0) and confirmation (sfx 8) instead of the menu blip.
- [x] Respawn waits include the source's `rand_n` jitter (exported in the trigger's former layer byte).

## Nineteenth pass (2026-10-05: level 2 as a classic racing road; details in `docs/PCE_CLASSIC_ROAD_20261005.md`)

- [x] The Wolf3D pair-character floor (128 coarse samples, ~15 updates a second) is replaced by a Chase H.Q.-style road: one static
      road picture in the BAT (148 characters, 1024 dots wide), scrolled sideways one scanline pair at a time (BXR) to follow the
      circuit and re-read through a second copy of its rows (BYR) for the road/kerb/sand stripes, which run with the distance
      driven. Full 512-dot resolution, the road moves every frame the loop completes (about 22 a second with seven rivals, was 15).
- [x] The sky is unchanged (its two scrolling bands); the haze rows now carry the road's far end.
- [x] The car keeps within 34 degrees of the circuit's direction; the track record's tangent is 256 steps a turn.
- [x] Cars, mines and shots project with the live camera (no more lag behind the last finished floor).
- [ ] Not done: roadside scenery; real hardware.
