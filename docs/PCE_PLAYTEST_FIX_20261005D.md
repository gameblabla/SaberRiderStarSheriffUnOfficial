# PCE playtest fixes, 2026-10-05 (fourth batch)

Built (`make -f Makefile.pce all`) and driven headlessly in the accurate-core emulator (screenshots, `sound_capture` spectra); nothing was heard by ear.

## Level 2 (the race)
* **Coming up from black.** `road_init` used to switch the display on with the road's first, still empty picture (a straight road at full brightness, no sprites) before `change_stage`
  reached `ui_black`. The display now stays off until the first frame is built; `ui_fade_in` turns it on (`main_pce.c` turns it on itself when the pause menu is left).
  The fade shows the real grid (HUD, cars) and the briefing box is up from the first frame.
* **A restart after losing a life** (`main_pce.c`): a death in the pursuit used to rebuild the grid with its briefing and then jump into the pursuit with no transition. The phase to resume
  (`resume_phase`: race phase or arena wave) is now read by `change_stage`, so the fade out, the stage card and the fade in lead straight to the pursuit (its music, no briefing).
* **Mountains.** The sine-wave ridge is gone. The PC's own mesa and boulder art (`assets/mode7.png`) stands in two hazed ranges along the horizon (`build_assets.py race_mountains`,
  `race_sky`): the far range pale, the near one warmer, both fading into the haze at their feet; hand-picked lattice colours keep the shaded sides brown. They sit in the sky's near layer
  with a hardware palette of their own (palette 9: the gradient shades that reach down there and six mesa colours; the clouds stay in the far layer), 245 of the 256 characters.
  The near layer now starts at scanline 56 (`irq.S`, was 80) and turns with the camera only (`road_pce.c`: it no longer slides when the car drives).
* **GO and the count.** 3, 2, 1 are a 660 Hz pip each and GO is a rising pip to 1320 Hz on a breath of noise: synthesised PSG scripts (`build_audio.py`, `psg.h` scripts 3 and 4; tones 20, 21).
* **The car standing still** keeps its two aerials: the straight-ahead frame's aerials are one source dot wide and the nearest-neighbour shrink to 64 dots dropped them (the turning frames' are
  thicker). The player has a fifth baked frame (`buggy_steer2`, `PCE_CAR_STEER+4`) shrunk so that thin lines survive; the rivals' cars are unchanged.

## Level 6 (Ramrod's arena)
* **A bolt hitting a Renegade** plays a PSG zap (tone 22, voice 1: noise and tone channel, no DDA): a 2.6 kHz tone sweeping down with a shimmer, on a burst of noise. The fist's hit keeps the PCM impact.
* **Rock and plasma ball.** Not changed: the PCE arena's rocks are only scenery (the PC's enemy plasma does not touch them either). A rock that vanishes next to a plasma ball is most likely the sprite
  table's shortage (rocks are drawn last and refused first); I could not reproduce one, so a savestate would help.

## Last level (the cruiser, `space_pce.c`)
* **HUD above everything:** drawn first, so it holds the lowest sprite slots and is admitted first on a full scanline.
* **The dialogues show Ramrod's ship** (`space_dialog_ship`, called from `story_pce.c draw`), as the other stages show their hero.
* **The cruiser moves more:** it sweeps 34-106 (a lap in 4 s, a pixel every step, it used to bob 48-64 at half a pixel) and rocks forward and back, the hull moving 0-20 dots to the right of its place (a lap in 3 s);
  both stop while the nose cannon gathers and fires. Everything that was fixed to the hull follows (gun ports, drone and mine bays, the hit test, the beam's hit and the gathering sparks use `space_hull_x`).

## Code space
`space_contact` moved to bank $72 and `boss_guns`, `port_shot`, `foe_in` to the cruiser's bank $77 (no overlay call between them and `boss_step` any more).
