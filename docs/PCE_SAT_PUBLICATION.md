# PCE SAT publication

SAT DMA reads one complete 256-word table from VRAM. Uploading into a source
that has already been submitted can expose partial entries or a stale tail
when a transfer crosses VBlank. Interrupt-safe VDC address setup alone does
not protect that source's lifetime.

The renderer now alternates sources at $7e00 and $7f00. `video_sat_target()`
waits for the previous submission's VBlank before returning the other source.
Every renderer upload includes all 64 entries, hiding the unused tail. Transfers
remain interruptible in the existing short bursts. The final VRAM write is
drained before `video_sat_commit()` publishes the source and captures both
scroll axes in one short critical section. VBlank uses those captured axes;
while an unfinished platform draw holds scroll, it holds both axes.

Direct SAT editors must copy the active source at `pce_sat_word` into the
returned target, finish every entry, then call `video_sat_replace()`. This
preserves the submitted scroll and any in-progress draw's scroll hold. Boss
pattern retirement and pause use this path. RAM `sat[1]` remains scratch;
VRAM source alternation does not change RAM `sat_page`.

Boss pattern changes happen before sprite admission. Retirement hides only
old entries using the replaced patterns or palette, then waits for VBlank
before uploading replacements. The new palette is queued after retirement.
Retirement may still intentionally hide affected old sprites while their
shared VRAM storage is replaced; this is distinct from publishing a partial
SAT. The arena HUD and bolts remain below $7e00.

Validation: `make -f Makefile.pce -j2`, including the build's ELF/bank checks
and disc packaging. No tests or emulator runs were performed at the user's
request. The supplied boss flash has not been verified at runtime.
