# SGX last-level fixes — 2026-10-11

Input: `sgx_last_issues4.txt`; the user confirmed that these reports concern the
last level (runtime stage 7). Starting revision: `b4400fc`.

## Changes

- Clip the falling cruiser one eight-pixel BAT row at a time as each row
  leaves y=224. Clearing only when the hull anchor reached y=224 allowed its
  lower rows to wrap through the 256-pixel BAT and reappear above. Row clipping
  applies only during death, preserving the normal fight's vertical movement.
- Track white-hit timers per enemy instance. Reserve sprite palette 30 (SAT
  palette 14) for white, and select it only for the struck actor's emitted
  entries, including VDC0 fallback. Keep the cache's original palette intact;
  ordinary colours return automatically when that actor's timer expires. Reset
  the timer when an enemy slot is reused. This also removes the old palette
  restoration code that invalidated a sprite cache slot to recover its colours.
- Save the actual cruiser scroll column for dialogue restoration. The old
  ternary expression selected a pointer to either camera; LLVM-MOS promoted
  `pce_scroll_x` into zero page but the indirect read used $0070 (I/O) instead
  of $2070 (RAM). The backup therefore used column zero while the greeting
  typed into wrapped cruiser columns. Restoring that wrong window left
  “Ramr” and “do” on the hull BAT, exactly matching the supplied save. Read
  the volatile scroll unconditionally before choosing the near camera so
  the compiler emits scalar loads. Check the saved column and the entire
  restored BAT for leftover glyphs.
- Retire the complete unused SAT tail on both VDCs and both alternating DMA
  sources in stage 7, as already done in stage 6. Stray entries outside the
  remembered active count can no longer survive successive draws. The existing
  empty sprite-record and descriptor no-op checks remain in use.
- At the start of the cruiser death sequence, clear all active escorts and
  projectiles, retire pending spawns and pickups, and reset their hit timers.
  The hull explosions finish before the victory story opens.

The supplied saves were decoded and their embedded screenshots inspected.
The cruiser save contains six leftover BG text cells: “Ramr” at row 13,
columns 1–4, and “do” at row 15, columns 1–2. The same six cells were reproduced
on the first patched build before the scroll-backup repair. The other save
shows Ramrod's arena; its dialogue close now has an explicit native comparison
of palette 31 against the baked HUD colours, and that check passes immediately
after close.
These files were inspected as evidence; they were not loaded as executable
states into a rebuilt image, since their old code/cache state is not a fresh
runtime for the new disc.

## Verification

- Retail SGX build: `make -f Makefile.pce SGX=1 all`.
- `test_sgx_last4.py`: native hit collision against two same-type gunships,
  separate SAT palette selection, flash expiry, unchanged HUD colours, stale
  SAT entries injected into all four DMA sources, and a compiled emitter call
  for the empty BG-only cruiser sprite record.
- Extended `test_sgx_final_boss.py`: greeting pages and restoration, explosions,
  the correct saved cruiser column, no leftover font cells anywhere in its BAT,
  falling-row wrap prevention, immediate enemy/projectile cleanup, absence of
  enemies/projectiles throughout death and victory dialogue, and completion of
  explosions before dialogue. Enemy fixture strides use the actual eleven-byte
  LLVM-MOS `Foe` layout.

Disc: `build/sgx/saber_rider.cue`.
Reports: `build/sgx/sgx-last4-verification.json` and
`build/sgx/final-boss-verification.json`.
Captures include `last4-hit-flash.png`, `last4-clean-sat.png`, and
`final-boss-offscreen.png` beside the disc. The captures were visually inspected.

Evidence is from seeded native execution in the accurate headless emulator,
with SGX enabled and hardware sprite limits. Physical hardware was not tested.

The new negative checks also ran against an isolated build of `b4400fc`, using
current baked SGX assets. That build fails on the unstruck gunship becoming
white, a lower hull row wrapping at anchor y=139, escorts surviving boss death,
and injected stale SAT tails surviving on VDC0. Before the scroll-backup fix,
the patched build reproduced precisely the six glyph cells in the supplied
cruiser save; afterward the full-BAT glyph check passes.

The first `test-sgx` invocation stopped at `test_sgx_cache_pressure.py` because
its captured numeric panel ID (32) now names a four-pattern portrait. It also
fails on the isolated original build. That harness now obtains the dialogue
ID from the current manifest while preserving the original 21-pattern,
six-page request, pinned allocations, and exact expected placement in pages
48–53. Frozen `--expect-refused` replays retain their original numeric ID.
The remaining suite commands are run in order as a continuation; the campaign
is rerun on the final rebuilt disc after the scroll-backup fix.

The final-disc campaign rerun and the dedicated arena HUD restoration check
passed. The standard PCE application also compiled and passed ELF/bank checks
after the final source change; this was an application build, not a regenerated
standard PCE disc. Failure evidence, reported thumbnails and focused logs are
retained in `artifacts/sgx-last4/`. Old Mednafen CD save states include the old
loaded program code; use a fresh session to test the rebuilt disc.

Final outcome: all 27 SGX suite commands passed across the initial run
and continuation, and the campaign passed again on the final disc. The late
stage, last2, last3, arena sprite, space planet, final boss and last4 checks all
passed on the final rebuilt image. Recorded hashes remained unchanged.
Suite logs and the final verification summary are in `artifacts/sgx-last4/`.
