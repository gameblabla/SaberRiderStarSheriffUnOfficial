# PC Engine stampede correction

The supplied `saber_rider.3cf2a969cfe3d3ab466e3792f7b9627e.mc0` reproduces partial
horse sprites. Its looping PSG gallop is active, rather than absent; an emulator
WAV capture also contains output. The platform draw had no world shake.

The spawn queue used 99-pixel spacing, putting more horse columns on the same
scanlines than the real VDC's 16 width units allow. The old equivalence tests
validated the renderer's partial admission behavior, and the 60 Hz test counted
only essential overflows; neither required every horse column to be present.
Even 192-pixel spacing loses a column at some horizontal phases beside a firing
hero. The queue now uses 256 pixels, retaining the original horse count and full
128x80 artwork. This makes the convoy more spread out and its passage longer.
No emulator sprite-limit bypass is used.

The platform draw translates every world SAT entry upward by 0–3 pixels and
commits the same background BYR offset after submitting the SAT. The HUD prefix
stays fixed. Background preparation preserves the previous displayed offset;
VBlank and HBlank use the committed value. The last herd frame returns to zero.
The firing-pose warmer moved to the herd's bank to leave space in the flow bank.

Gallop conversion now retains bass down to 20 Hz instead of filtering below
80 Hz, and uses unity gain instead of 0.7. The original packed-game sample
82EFBA26 still loops on its independent PSG channel, concurrent with firing and
CD-DA. Horse/player contact also plays the sampled impact along with the hurt
voice. Existing sample-rate, stop, loop, bank-crossing and voice checks pass.

`test_herd_visibility.py` executes 1,024 complete native platform draws: all
256 horizontal phases for all four firing heroes. It requires exact complete
horse X/Y cells, checks all shake offsets and a fixed HUD, and counts the final
SAT's real scanline widths (peak 13/16). This catches the original defect instead
of accepting partial admission. It is included in `test` and `test-herd`.

Review captures are in `artifacts/pce-stampede-fix/`: the supplied state's
screenshot/audio and fresh rebuilt-disc gameplay screenshot/audio. These are
representative scenes, not pixel-identical before/after frames. The old state
contains the old program in CD-loaded RAM; loading it on the new disc restores
that old code too. Verification boots the rebuilt disc and drives native herd
triggers. Start the new disc or use its in-game checkpoint, rather than loading
that old emulator state.

Build: `make -f Makefile.pce all`. Regression log:
`build/pce/stampede-regression.log`. Native visibility report:
`build/pce/herd-visibility-verification.json`. Herd performance reports:
`build/pce/gameplay-profile-herd{1,2,3}.json`.

Final verification: the full `make -f Makefile.pce test` exits successfully,
including all campaign, foreground, presentation, dialogue, audio and codec
checks. Each of the three convoys presents 300/300 new frames with held fire,
zero essential overflows, and no firing-time pattern uploads. The separate
visibility sweep also passes with all four corresponding HUDs. These are
accurate-core emulator checks; physical-console behavior was not measured.
