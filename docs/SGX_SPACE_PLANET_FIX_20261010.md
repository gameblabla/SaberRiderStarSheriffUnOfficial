# SGX final-stage planet

Stage 7 keeps the repeating nebula on VDC1 and the introductory planet on
VDC0. VBlank previously left VDC0 at the nebula's scroll position, even though
the dialogue raster path used the independent near-layer position. That made
the planet return at the 512-pixel BAT wrap, with a visibly cut edge.

VBlank now publishes the near-layer scroll for VDC0 before the cruiser loads.
Fully offscreen planet columns become transparent before the hardware can
wrap them into view. Near scrolling stops at pixel 264, beyond the 260-pixel
artwork, and retains that empty window if the flight clock rolls over.
Dialogue/power BAT restoration reapplies the offscreen clearing. Loading a
new stage resets the column state; cruiser rendering retains its own scroll.

`tools/pce/test_sgx_space_planet.py` loads stage 7 through the native transition,
then freezes gameplay ticks and invokes compiled draws at selected flight
times. It checks both VDC scroll registers, visible BAT entries and patterns,
the initial planet, empty space after exit, native BAT restoration, clock
rollover, and SGX code integrity. It is included in `Makefile.pce:test-sgx`.

The pre-fix SGX disc fails the hardware scroll check at near position 8:
VDC0 and VDC1 both report 32. With that check omitted, it also fails at near
position 257 when BAT column 0 reappears at the right edge.

Rebuilt debug SGX image and the focused planet and `test_sgx_last3.py` checks
passed. Reviewed emulator captures at near positions 128, 257 and a later
flight time show the planet leaving left, then only space. Captures and the
verification JSON are under `build/debug-sgx/space-planet-*.png` and
`build/debug-sgx/sgx-space-planet-verification.json`.

These are seeded native emulator checks, not a physical-console test.

The release SGX image also rebuilt successfully and passed the focused planet
check and the complete `test_campaign.py --sgx` scenario check, including the
cruiser, ending, menu return, and hero selection. A campaign run on the
level-select image reached the cruiser and ending
but timed out on its return-to-menu expectation. A supplemental `test_port.py`
run, which does not force SGX mode, failed its walking-distance assertion; it
is not counted as SGX validation. Logs for this work are `/tmp/sgx-planet-*.log`.
