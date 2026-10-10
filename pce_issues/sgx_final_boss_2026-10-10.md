# SGX final boss fixes — 2026-10-10

The cruiser previously replaced the departing planet's BG0 patterns and BAT,
stopped updating the nebula's BG1 column cache, and left a 16-pixel gap after
its cropped rear. Stage-7 text also addressed columns using the planet scroll
after the cruiser had taken ownership of BG0. The death tick immediately
requested dialogue, freezing the final fireball and retiring its sprites.

The SGX asset bake now provides eight narrow planet sprite strips with one
shared palette. Visible strips remain behind the cruiser's opaque BG0 pixels
until their final pixels leave. Stage-7 SGX dialogue uses BG panel tiles and
four sprite corners so the planet also fits behind the greeting. Closing the
panel restores the original font. The cruiser starts at x=32, with collision,
cannon, port and enemy-release coordinates adjusted to match. Its drawing
continues streaming the repeating nebula.

The SGX death sequence retains the hull for 72 ticks of sprite explosions,
removes it, then allows 36 more ticks for the final fireball to finish before
opening victory dialogue. BAT restoration cannot recreate a destroyed hull.

Validation on the rebuilt retail disc, using the accurate headless emulator
with SGX enabled and hardware sprite limits:

- `make -f Makefile.pce SGX=1 all`: passed ELF/bank checks and disc generation.
- `tools/pce/test_sgx_final_boss.py --out build/sgx`: passed. The arrival retains
  16 planet SAT entries; the greeting retains nine. Visible text, the rear's
  placement, nebula source-pattern equality across BAT wrapping, explosion SAT
  entries, hull removal and completed effects before dialogue are checked.
- `tools/pce/test_sgx_space_planet.py --out build/sgx`: passed scrolling,
  one-shot retirement, power-panel BAT/font/palette restoration and clock wrap.
- `tools/pce/test_campaign.py --out build/sgx --sgx`: passed through the ending,
  credits/front end and new-game lifecycle.
- `test_port.assets(build/sgx)`: passed generated-asset integrity checks.
- `make -f Makefile.pce SGX=0 all`: passed; the extra post-explosion wait is
  compiled only into the SGX version to preserve the standard PCE bank budget.
  The final SGX ELF files match the tested files by SHA-256.

The generic `test_port.py` gameplay runner was also attempted against the SGX
disc; it does not enable SGX and failed its walking assertion. SGX gameplay
validation above uses the explicit SGX campaign runner. The full `test-sgx`
suite and physical hardware were not exercised.

Review captures and reports are in `build/sgx/final-boss-*.png`,
`build/sgx/final-boss-verification.json`, and
`build/sgx/sgx-space-planet-verification.json`. These are seeded native emulator
scenarios and inspected captures, rather than a manual campaign playthrough.
The rebuilt disc is `build/sgx/saber_rider.cue`.
