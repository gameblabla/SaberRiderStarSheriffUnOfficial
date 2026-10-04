# PC Engine: stage 7 ships, HUD and explosions, race steering, background streaming — 2026-10-04 (fourth revision)

Follows `PCE_PLAYTEST_FIX_20261004C.md`.

## Stage 7 (the space flight)

- **Ships face the way they fly.** The fighters and gunships were drawn mirrored; the art already faces left (`space.c` draws a ship
  unflipped unless it comes from behind), so `space_pce.c` now draws every foe unflipped.
- **Explosions.** A destroyed ship bursts in the source's fireball frames (`expl`, 30 steps, five visible frames, the sixth is a fade
  too faint to keep): a 48-dot fireball, with two 32-dot ones round a fighter and two more big ones round a gunship (`space.c blast`).
  The art is sprite ids 12-21 (`build_assets.py`), so the HUD pieces start at 22. `expl` lives in the work bank (console RAM is full).
- **Sounds.** A fighter or gunship plays the enemy's yell followed by the explosion (tone 8, the CD ADPCM `enemy_death` event: the
  source's sfx 5 then sfx 6); a mine only bursts (the PCM `impact`). The cruiser breaks up as `PH_BOSS_DIE` does: its fire stops, 72 steps
  of fireballs burst over the hull to the big bang (tone 17, the wreck's sample) and then it is gone and the closing dialogue starts.
- **HUD.** One narrow column at the top left, ending at y 90, with at most one sprite on any scanline (it was up to six): shield cells,
  then one piece each for lives (`x3`), gun power (three pips), torpedoes and hero-power stars (icon and count). The labels (RAMROD, PWR,
  TRP, SPC, CRUISER) are gone. The cruiser has a vertical hull gauge at the right edge (four 16-dot segments, 64 dots for 1200 points;
  it used to show only a label and no bar). All pieces are built in `hudart.py space_hud`; `hud7_draw` is in bank $70 for room.
- **Background.** The nebula scrolled `(clock >> 3) & 255` over a 512-dot picture: its second half was never shown and the picture
  jumped back after 2048 steps (a 31-column reload in one frame). It now scrolls the whole 512 dots and wraps by itself.
- **Code placement.** `space_frame` (drawing) moved to bank $7c, the steps stay in $73; `race_box` moved to bank $6f. Bank $73 was
  654 bytes over, $7c 115 over.

## Background streaming (all scrolling stages)

A tile slot whose last column scrolled out was reusable at once, but the new scroll only takes effect at the next VBlank, so on busy
frames (several columns a step, nearly full tile cache) the left edge column could be drawn with another column's tiles. A released
slot is now held until the next `video_background` call (`HOLD`, 48 slots; a full cache gives its held slots up rather than failing).
I could not reproduce the glitch with the checks available (the display is correct at every frame end on stages 1, 3, 4, 5 and 7 while
scrolling), so this is a fix for the one race I found in the streaming code, not a measured cure; no savestate of it was supplied.

## Race (stage 2)

- **Steering animation.** The buggy's five steering poses (hard left .. hard right) were baked but never shown: the lean was updated as
  `tilt += (steer * 7 - tilt) / 8` in whole numbers, which is 0 for any `|steer * 7 - tilt| < 8`, so the lean never left 0. It is now Q4
  (`steer * 112`, thresholds at 24 and 80 for 1.5 and 5 of the source) and, as in `mode7.c render_player`, the car also slides sideways by
  1.2 x the lean. The spin-out on a mine is unchanged.

## Verification

Screenshots on the rebuilt disc: stage 7 ships, explosions at five frames, cruiser gauge and break-up; race left / straight / right.
Frame-by-frame check of the visible background cells against the scene map on stages 1 (during a herd), 3, 4, 5, 7 (across the wrap).

Test status: `test_port`, `test_campaign`, `test_boss_presentation`, `test_boss_passes`, `test_herd`, `test_presentation`,
`test_dialog_restore`, `test_audio` and the three 60 Hz herd profiles pass. `test_herd_render`, `test_herd_visibility` and `test_foreground`
fail; they pass on the last commit and fail the same way on the previous pass's uncommitted state without this pass's changes
(see `PCE_ISSUES.md`).
