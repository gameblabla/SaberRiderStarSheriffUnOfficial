# PCE WIP issues — user playtest, 2026-10-03

Baseline preserved in **`9d04f6e`**, committed as WIP before corrective work.
Implementation status below does not replace the user's visual/audio acceptance.

- [x] Restore title screen and character selection before gameplay. Original
      title art, all four portrait panels, real input selection, explicit save
      continuation; a new game returns through this flow after game over.
- [x] Add the opening mission briefing as original still artwork and paged text.
      No video playback added.
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
