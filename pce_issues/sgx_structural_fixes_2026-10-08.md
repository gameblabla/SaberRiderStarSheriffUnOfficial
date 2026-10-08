# SGX structural repairs following sgx_issues.txt

These changes build on the working tree from the referenced session. The older
rendering report describes that session's checks, not validation of this change.

## Resource ownership and presentation

- Foreground uses sprite palette 28, horses and the stage 3 moon use 29,
  platform boss hulls use 30, and the HUD uses 31. Cached actors use 16–27.
  Foreground uploads and boss hit flashes no longer write the same palette.
- Sprite cache flags track whether each pattern upload actually exists on
  VDC0 or VDC1. New world poses upload directly to their drawing VDC; another
  copy is uploaded only when a draw needs it. This removes the compulsory
  duplicate transfer for every enemy animation cache miss.
- Actor-plane preferences and split-pass metadata have explicit storage in
  the always-mapped work bank rather than anonymous bytes in transfer scratch.
- An uploaded second SAT is marked ready. Both SAT pointers are committed
  together before marking the second SAT pending for VBlank. A VBlank during
  sky streaming cannot consume an uncommitted second SAT.
- Platform dialogue uses the same dual-VDC actor pass as play. It retains the
  old actor plane until both replacement tables are submitted, preserves the
  dialogue prefix above the foreground, and uses the scrolling world BAT for
  panel placement and restoration in stages 4 and 5.
- Boss pattern changes retire only conflicting entries from VDC1's displayed
  SAT, wait for their retirement, then replace its hull patterns. VDC0 scenery
  patterns and unrelated sprites remain intact.
- Both flying boss kinds submit their laser pass. The previous SGX pass
  excluded the level 1 gunship's shots.

## Background assets and streaming

- The level 1 sky advances at a quarter pixel per video tick, including while
  the camera is still. Its complete panorama remains in Arcade RAM; wrapping
  and fractional scroll are handled by the streamer.
- Horse scenes limit the VDC1 character cache to 512 entries, ending at $2800.
  Resident horse poses begin there and cannot be overwritten by sky eviction.
  The baker budgets those sky windows for the smaller cache.
- Stage 3 loads the PC release's native night-sky and red-moon PNGs. The sky
  repeats at approximately 0.05 camera speed; the moon uses resident 32×32
  sprite cells at approximately 0.02 camera speed behind other actors, with
  its own palette. Its VRAM reservation is excluded from the sky cache.
- Stage 4's FarMountains layer (the blue bushes) is removed from the main
  playfield bake and retained on VDC1. The sky and bush bands have separate
  Arcade map records, 33-column windows and fractional scroll values, sharing
  one character directory. A raster change at line 64 selects the bush
  band's approximately 0.10 camera speed below the 0.03 sky band.
- Pursuit and race-boss music requests occur when the respective post-dialogue
  phase begins. The race-boss track is requested once on the first playing
  frame after its dialogue closes.

## Build validation

`make -f Makefile.pce SGX=1 all` completed successfully, including the ELF
bank/symbol checks and disc packaging. The packed sky record has a compile-time
size assertion; the ELF checker guards the new overlay call destinations and
resident optional-sprite admission.

Output: `build/sgx/saber_rider.cue`.

No tests, emulator runs, profiling, screenshots, playthroughs or hardware checks
were performed for these changes. Runtime behavior and crowded-scene frame
rates are unverified.
