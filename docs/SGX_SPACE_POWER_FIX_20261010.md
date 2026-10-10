# SGX final-stage special-move tiles

The supplied `specialattack_tileglitches.mc0` screenshot shows wave/name
fragments beside the planet. The special-move band is written into VDC0's
BAT, but used `pce_scroll_x`, the nebula's VDC1 camera before the cruiser
loads. VDC0 displays the independent `pce_sky_near` camera. Consequently,
some band columns were outside the displayed window and the planet remained
visible where the wave should have covered it.

The cut-in now places those columns using the planet camera. After the
cruiser loads it continues using the hull's existing scroll. Cleanup now
calls `space_hull_bat` for SGX flight as well as the cruiser; that existing
dispatcher restores the planet BAT and reapplies retired-column clearing.
Previously, flight cleanup reloaded only the VDC1 nebula and left the
temporary cut-in cells in VDC0.

`test_sgx_space_planet.py`, already included in `test-sgx`, now invokes the
compiled cut-in with interrupts running at planet positions 128, 257 and
264. It requires all 462 wave cells in the displayed window and exact
restoration of the complete planet BAT and font, an unchanged nebula VRAM,
and restored background palettes. It retains the planet exit, wrap and
clock-rollover checks. These are seeded native emulator checks; the supplied
save's embedded screenshot was inspected, rather than loading its old code
over the rebuilt application.

Before the fix, the release image fails the wave-window assertion at position
128. The previous debug image also fails the complete BAT restoration check
when the wave-window assertion is bypassed. The rebuilt release SGX image
passes both checks. Reviewed captures show the full cut-in and the restored
planet without wave or text fragments. Evidence is in
`build/sgx/space-power-*.png` and
`build/sgx/sgx-space-planet-verification.json`.

Validation passed: `make -f Makefile.pce SGX=1 all`,
`python3 tools/pce/test_sgx_space_planet.py --out build/sgx`, and
`python3 tools/pce/test_campaign.py --out build/sgx --sgx`. The campaign
includes the cruiser's special move, ending and return to the front end.
The full `test-sgx` suite and physical hardware were not run for this change.
