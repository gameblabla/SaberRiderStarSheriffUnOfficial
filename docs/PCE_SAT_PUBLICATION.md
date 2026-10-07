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

Boss pattern changes happen before sprite admission. At arrival, `hull_claim`
reserves pages 16–39 and palette 14, invalidates displaced cache IDs, and
rebuilds a complete non-boss frame in the remaining cache. Other pages of
displaced owners remain protected until that SAT has reached VBlank. Only
then are their old pages released and the hull patterns/palette overwritten.
The old retirement-only path hid the hero for two frames when its displayed
patterns occupied the hull's pages. A complete SAT alone did not prevent that
deliberate hole in the picture.

Both full-size gunship rider poses now share one baked palette and pattern set,
so changing the rider's aim only changes the piece list. Size/layer and
Hyperjumper side/front changes still retain the defensive retirement fence;
those transitions occur off screen. The arena HUD and bolts remain below $7e00.

Validation (2026-10-07): `make -f Makefile.pce -j2`, including ELF/bank checks
and disc packaging. A focused fresh-disc accurate-core replay seeds the live
hero's cache patterns into the hull's range during the native arrival music
seek. The preceding build loses all hero entries in the hardware SAT for two
frames; the repaired build retains them throughout 90 video frames, completes
arrival and uploads the far hull. Before/after screenshots were visually
reviewed; evidence is in `artifacts/pce-boss-cache-handoff/`. Both stage 1 and
stage 5 rider records were checked for shared palette/pattern addresses.
`tools/pce/test_boss_cache_handoff.py` preserves this focused check. The broad
suite was not run.

The supplied `boss-flash.mc0` stalls in a CD BIOS seek in the headless core.
It also restores an older application RAM/code layout; loading an old state
over a newly built disc does not replace the code captured in that state.
Verification therefore used a fresh boot and the seeded cache placement above.
