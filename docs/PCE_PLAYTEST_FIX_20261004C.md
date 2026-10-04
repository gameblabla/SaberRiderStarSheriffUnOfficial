# PC Engine: victory lettering, backgrounds, dialogue, race spin, stage 3/4 — 2026-10-04 (third revision)

Follows `PCE_BOSS_VICTORY_RACE_FIX_20261004B.md`. Source of the requests: the two supplied states
(`saber_rider.a9ca666ccf0a1fe0b3c7d2602750f538-missing_camera_tile.mc0` and `…-bad_background_graphics.mc0`) and the playtest list.

## Front end

- **MISSION ACCOMPLISHED** is no longer light added to the painting. The painting uses all sixteen BG palettes (no palette is
  reserved for a blend, nothing under the lettering is darkened) and the lettering is opaque 16x16 sprites with one sprite
  palette, on top (`frontend.py` `victory_screen`, piece table in the screen's extra blob, drawn by `victory_pce.c`). The two
  lines are laid out so that their sprite rows never share a scanline (MISSION's rows end at its baseline, ACCOMPLISHED's
  begin at its top): 6 + 12 pieces, at most 12 of the 16 width units on any line. GAME OVER keeps its pulsing light.
- **NOW LOADING** is back on the stage title card: it appears under the sub-title once the stage number, name and sub-title are
  complete and stays through the disc read (`card_pce.c`; the strings are one blob and the card shares bank $79).

## Platform stages

- **Camera tile (stage 1).** The camera's cell used to carry a patch of wall because the background has sky at that spot; the
  previous revision made the patch transparent, which left a hole beside the camera. The wall is now part of the background
  (`camera_wall_background`: the same wall as directly below the cell, only over the sky pixels the cell covered) and the cell
  stays transparent around the bracket.
- **Background seams.** Layers with a parallax below 1 (hills, `MidBG`, `Cars MidBG`) were drawn per 256-dot span as seen from the
  span's first camera position, so they jumped by `(1 - parallax) * 256` dots at every span boundary: a vertical seam through a
  building and a second copy of the van's nose at x 8192 (the supplied state). They are now laid as one continuous image
  (`continuous_layer`): the layer shifts one dot a dot and takes its steps where the layer is empty, or where fewest dots would
  repeat. Stage 5 needed 20 cells merged to fit the tile cache after this; stage 4 went from 1338 to 952.
- **Dialogue boxes.** The inside of every box is one flat colour (`presentation.dialog_box`), so the opaque glyph cells never
  clash with the source's vertical gradient. Border and corners are unchanged.
- **Stage 3.** It opens as the source does (`game.c walk_in`): the hero starts at x 8 with the camera at 48 (off screen), walks right
  by itself to x 144 and then the radio scene opens (`play_pce.c`; the dialogue no longer opens before the hero is visible). The
  background layers are in the night colours of `night.c night_layer_tint` (`NIGHT_TINT`).
- **Stage 4.** The foreground layer is removed like on stages 1 and 3.
- **Hyperjumper (stages 3 and 4).** Its front pose (phases 9-11: drops in facing the hero, fires down, leaves upward) is a fourth
  hull record in Arcade RAM (`boss_big`, front_idle at 0.8 so that its pieces fit the same 96 patterns). It replaces the side hull
  in the pattern pages while the ship is off screen above the arena (phase 8 -> 9 and 11 -> 3), `hull_level` 3 in `boss_pce.c`; the
  wreck keeps the pose it was hit in. The hull's draw routine moved to bank $74 for room.
- **Speaker portrait palette.** Sprite cache slots 15 and up share one hardware palette; a later portrait overwrote it, so a
  cached speaker (April, after Claudia) came back with the wrong colours. `story_pce.c` loads the speaker's own palette when each
  page is drawn.

## Race (stage 2)

- The speedometer is gone.
- **Spin-out.** A mine hit spins the buggy one whole turn, easing out (the source's `render_player`). 11 poses of the car, rotated
  at its true shape and stretched to the 512-dot clock, each in two sprite objects (`buggy_spin*_left/right`); the pose follows
  `spin` as `360 p (2 - p)`.

## Verification

Screenshots from the rebuilt disc: victory (`vic-1.png`), card, race dialogue pages, spin poses, stage 3 walk-in and night, the
boss' phases on stages 3 and 4 (hull level 3 during 9-11), and the camera/background composites of the two supplied states.
The states themselves restore their old program; start the rebuilt `build/pce/saber_rider.cue`.
