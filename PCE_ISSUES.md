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
