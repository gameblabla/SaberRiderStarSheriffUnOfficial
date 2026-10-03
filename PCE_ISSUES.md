# PCE WIP issues — user playtest, 2026-10-03

Baseline is a WIP, not source-game presentation or audio acceptance.

- [ ] Restore title screen and character selection before gameplay.
- [ ] Add mission briefing still screens (no video requested).
- [ ] Replace synthesized shooting effects with PSG DDA samples: 5-bit PCM
      around 7 kHz is the minimum preferred implementation. Investigate
      `../PCE/Sound/adpcm_build_14_2bit.pce` and create a matching compressor
      before claiming its 2-bit ADPCM option is implemented or its CPU budget met.
- [ ] Remove synthesized jump effect; retain the character jump voice.
- [ ] Fix corrupt/noisy audio during death, scene reloads and game over.
- [ ] Convert foreground occluders into sprites instead of masking every actor.
- [ ] Match Saturn dialogue layout and include original 2D portraits.
- [ ] Lock platform background vertical scroll; fix its visible glitching.
- [ ] Restore diagonal up/down aiming animations in both hardware-flipped facings.
- [ ] Replace level 1 and 3 moon/sky art with a blue gradient, releasing tiles
      for other background elements.
- [ ] Rebuild top-left HUD to resemble Saturn, including selected hero icon.

Validate changes with the specified accurate Mednafen PCE core and Super CD v3
BIOS. Seeded regression results are separate from visual/audio playtest acceptance.
