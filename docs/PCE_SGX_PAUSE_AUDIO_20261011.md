# PCE / SGX pause and boss audio

Bosses 1, 3 and 4 use PSG voice 1 (noise channel 4, tone channel 3)
for their guns. The hit zap (effect 22) previously replaced that same
script. It now uses voice 0 (noise channel 5, tone channel 2), alongside
the player's shot, leaving the boss voice independent of hit feedback.
All six PSG hardware channels already have owners: channels 0/1 are
DDA effects/loops and channels 2–5 form the two PSG effect voices.
This reuses the existing voices without adding an interrupt-time mixer or
stealing a DDA loop channel.

PAUSE now chooses a sprite palette absent from the retained VDC0 SAT
(VDC1 is hidden during SGX pause), saves its displayed colors, and sets
explicit white fill / black outline. It reads and restores the actual VCE
rather than the held flash snapshot. Each color access has a short IRQ
critical section. If all sixteen sprite palettes are occupied, it retains
the old HUD-color selection without recoloring displayed sprites.

Unpause publishes the saved gameplay SAT and waits for its hardware
replacement before restoring the borrowed font patterns and palette.
Previously the font upload happened while PAUSE still referenced those
patterns, displaying parts of the font in the pause rectangle.

Palette selection runs in flow bank $6e; pattern/SAT work remains in
SGX bank $82. Both builds pass their linker memory checks.

Validation on rebuilt retail discs, using the native headless emulator:

- PCE and SGX builds passed.
- `test_pause_audio.py` passed for both targets: ordinary font restoration,
  explicit white/black with all palettes black and palette updates held,
  per-video-frame pattern lifetime, palette restoration, and concurrent
  boss gun scripts 13/14/15 with hit script 22.
- PCE `test_audio.py` and `test_dialog_restore.py` passed.
- SGX `test_sgx_reported_issues.py`, `test_herd.py --sgx`, and
  `test_sgx_arena_saves.py` passed, including HUD palette preservation,
  both herd rows after unpause, and arena pause backdrop preservation.
- `make -f Makefile.pce test` stopped in `test_port.py` at
  "Walking uses elapsed video ticks". That fixture's diagnostics helper
  assumes the non-retail RUN menu; the default retail build uses RUN to
  pause. The full suite did not complete.

Not tested on physical hardware.
