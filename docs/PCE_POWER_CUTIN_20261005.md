# PCE: hero power cut-in, level 2 sky, race placing (2026-10-05)

* **Hero power** (levels 1, 3, 4, 5 and the cruiser): no video. The world dims, a blue wave wipes across the whole screen width (BG characters in
  palette 15 over BAT rows 7-20, crests flowing by rotating the palette), the hero's portrait slides in at the left, the name (white, black outline, a
  sprite) at the right, then a white-out and the power lands. `src/platform/pce/power_pce.c` (bank $7b, `power_frame`); assets `power_wave`
  (build_assets.py) and the `pwr*` portrait/name sprites (presentation.py, one palette a hero). The strike itself is unchanged
  (`combat_tick` / `space_tick` run it when `pce_power_land` is set). The font region is overwritten while it runs and put back.
* **Buttons**: `main_pce.c read_pad` reads the pad twice a frame; a 6-button pad answers one read with its extra buttons (direction nibble 0000,
  III-VI in the low nibble). III = power. On a 2-button pad a tap of Select (<= 20 frames, no direction held meanwhile) is the power; Select held
  with a direction is still the aim. Tested with a Mednafen build that accepts the pad's mode bit (mask 0x1000).
* **Level 2 sky**: the sky and its mountains now reach scanline 127 (the road's far rows were fully hazed there: a flat band); the road's raster
  starts at scanline 128 (`irq.S`, rows 8-55). The road picture's haze rows are no longer uploaded.
* **Level 2 placing**: the placing at the flag is computed from fresh gaps (`field_rank_call`; the gaps the loop keeps were up to three steps old), and
  the "every rival has finished" loss needs at least three rivals still running (with fewer left the car is in the first three).
