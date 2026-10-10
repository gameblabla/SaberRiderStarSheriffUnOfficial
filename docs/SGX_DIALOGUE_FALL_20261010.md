# SGX dialogue lifecycle and cruiser fall

The stage-6 close path now publishes and waits for a SAT without the panel
corners before restoring the font and clearing the panel BAT. Previously the
BG panel was cleared while its rounded sprite corners remained displayed.

Stage 7 keeps the displayed sprite generations when opening dialogue. It no
longer submits three empty SATs or hides VDC1 sprites. The frozen world remains
visible while the portrait, corners and ship/planet SAT is prepared.

The first stage-7 dialogue page snapshots the 28-by-6 covered BAT cells.
Subsequent pages replace the panel directly. Closing publishes the ship/planet
SAT without the corners, waits for VBlank, then restores the covered cells.
The whole cruiser BAT and the sky cache are no longer invalidated by dialogue
page changes or close. The greeting-to-fight transition also avoids rebuilding
the restored SGX hull BAT.

The snapshot and its save/restore routines live in overlay bank $6e, using
336 bytes outside the nearly full stage bank. Restoration calls the existing
column-wise panel writer through its banked entry point. Greeting completion
uses a bank-$71 helper to keep the flight draw bank within its limit.
Snapshot reads directly select the VDC read address and read both bytes with
interrupts disabled, then restore the previous register selection. Calling
the general register helper inside that section would re-enable interrupts.

During the 72-tick cruiser explosion, its anchor descends by 2 through 5 pixels
per tick; new bursts use that same anchor. At y=224 the renderer retires the
hull BAT so hardware scrolling cannot wrap it back into view. The existing
post-explosion interval completes before victory dialogue begins.

Verification is performed exclusively by the Luna-6 agent with high reasoning.

- `make -f Makefile.pce SGX=1 OUT=build/sgx all`: passed, including ELF bank
  checks, arena image generation and disc packaging after the atomic-read fix.
- `python3 tools/pce/test_sgx_final_boss.py --out build/sgx`: passed.
  The probe checks 288 video frames around greeting entry and the four greeting
  pages. Each page retains nine planet entries and unchanged VDC1 VRAM. Close
  retires four corners and restores the covered panel cells. The death probe
  checks downward motion against elapsed death ticks and verifies VDC0's BAT
  clears at the bottom edge before the death timer ends.

Disc: `build/sgx/saber_rider.cue`. Final-boss results and captures:
`build/sgx/final-boss-verification.json` and `build/sgx/final-boss-*.png`.
The instrumented `python3 tools/pce/test_sgx_last3.py --out build/sgx` probe
also passed. It checked the stage-6 panel across seven pages, captured four
corner SAT entries on the final page, and confirmed they were absent when the
arena BAT returned four frames after close began. The same focused probe passed
its forest parallax samples, two-large-robot compositor check, and later
dialogue/robot restore check. Report and captures are in
`/tmp/sgx-dialogue-luna-high/`. These are focused emulator checks, not a full
suite or a physical-hardware run.
