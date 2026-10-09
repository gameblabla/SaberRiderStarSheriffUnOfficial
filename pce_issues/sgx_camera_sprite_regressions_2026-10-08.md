# SGX camera and sprite regressions — 2026-10-08

Follow-up to `current_sgx_issues.txt`. Earlier SGX notes describing sky drift
or reversed player-based motion do not describe this implementation.

## Repair

- The sky and stage 4's lower parallax band use the foreground camera of the
  same submitted draw, with their own Q8 camera ratios. Walking inside a fixed
  camera view cannot move either background. Source columns repeat modulo
  their panorama width while BAT columns advance continuously across seams.
- VDC0 and VDC1 have separate sprite pattern addresses and page ownership on
  platform stages. They still share palette slots, descriptors, and the pins
  protecting the last two displayed generations. A mirrored hero or fallback
  actor allocates and uploads to its destination VDC before emitting entries
  that reference that VDC's pattern address.
- The platform foreground is admitted on VDC0 before optional actors and
  projectiles retry there. Their changing scanline use cannot steal previously
  admitted foreground pieces. The foreground is emitted once per draw.
- Boss pattern claims affect VDC1. VDC0 foreground does not lose its cache
  pages or retained list when the hull changes. The horse pose reservation
  ends when the platform boss begins, after the final convoy, so the boss and
  its shots do not compete with an obsolete horse allocation restriction.
- The extra VDC1 addresses live in always-mapped CD-RAM bank $68, with the
  second ownership table in console RAM. The ELF checker continues to guard
  bank capacity and console state against the test trampoline/software stack.
  The upload helper and SGX hull drawing use bank $80; fixed helper destinations
  are checked during the build.
- First sprite uploads retain the direct descriptor/upload path. Race, arena
  and space mirrors keep their shared addresses; only platform mirrors run the
  second-plane allocator. The SGX fade body moves to a checked $80 overlay to
  make room in the resident renderer, with a resident wrapper for its callers.
- Space-stage dialogue panels may use the six unused sprite pages at
  `$7800..$7dff`, immediately below the alternate SAT at `$7e00`. Other space
  sprites keep the old limit. A captured failing frame had an empty descriptor
  slot and sufficient SAT/scanline space, but every six-page block below
  `$7800` overlapped displayed/pinned patterns; the six free tail pages were
  excluded. Reserving that admission path removes dependence on fragmentation
  caused by preceding flight/dialogue generations.

The ordinary PCE renderer retains its existing allocation path. Existing
unrelated worktree changes are preserved.

## Verification

Binary and emulator verification is assigned to GPT-6-Luna at xhigh, using
the accurate Mednafen core with hardware sprite limits enabled.

`python3 tools/pce/test_sgx_motion.py --out build/sgx` passes on the rebuilt
image. A stationary player at x=4220 lets the native camera catch up to 4100;
the sky reaches 1025, exactly `ceil(4100 * 64 / 256)`. A left walk moves the
player from x=4200 to 4174 while camera=4100 and sky=1025 remain fixed. The
same harness fails on the preserved pre-fix binary: at camera=4100 it reports
sky=2874 instead of 1025. Every one of the 990 cells in each inspected sky
window matches the archive pattern bytes and BAT palette, including the
level 4 split band. Levels 1, 3, 4 and 5 are covered.
The strengthened run checks windows crossing the 512-pixel BAT boundary:
stage 1 camera=4000, stage 3 camera=6000, stage 4 camera=5000 (lower band),
and stage 5 camera=6464. Every sampled window still matches all 990 source
cells, including stage 4's independently scrolling row bands.

`python3 tools/pce/test_sgx_rendering.py --out build/sgx` passes. Its native
SAT/VRAM checks cover stage 1/3/4/5 sky cells and retained foreground pieces,
30 consecutive hero-bullet presentations, and all six walker death poses
with their source pattern bytes and palettes. The measured idle and firing
cadence is 60 presentations per second in this fixture. See
`build/sgx/rendering-verification.json`.

`python3 tools/pce/test_herd.py --out build/sgx --sgx` passes all three
level-1 convoy seeds with held fire. The strengthened checks preserve all
five resident horse poses before/during/after the convoys, verify source
pattern bytes and hardware SAT sizes/palette, and match retained foreground
entries and live hero bullets against published SATs. A shared 0–3px herd
shake is accounted for explicitly; X, pattern and palette must still match.
The three fixtures check 60, 20 and 65 retained foreground parts and 4, 5 and
5 live-bullet samples respectively. Their measured cadence is 56.52, 56.47
and 56.87 presentations per second, so these checks do not claim all convoys
run at 60 Hz. Post-convoy camera catch-up checks the loaded right edge and
hardware scanline limits. See `build/sgx/herd-verification.json`.

`python3 tools/pce/test_sgx_regressions.py --out build/sgx` passes on the final
candidate. At the native locked arena camera=9697, all 18 VDC1 hull parts
remain present in all 60 sampled presentations, with source pattern bytes
checked at the first and last sample. All 240 retained foreground-part checks
match the published SAT and the foreground palette stays unchanged. Hero and
boss projectiles appear in 59 and 48 presentations respectively; 61 and 79
persistent on-screen projectile candidates have zero SAT misses, using each
VDC's own pattern addresses and copy-valid bits.

An ordinary projectile executes native boss damage (HP 10→9). Palette 30
turns white and restores from the archive; palette 28 stays unchanged. The
captured hull has 6,784 white pixels in the next emulator frame. A palette
change does not require a new SAT generation, so the fixture checks the
actual rendered frame rather than treating `pce_presented` as a palette
publication counter. See `build/sgx/sgx-regression-verification.json` and the
three captures in `build/sgx/sgx-regression-evidence/`.

The final candidate builds with all ELF layout guards passing. Its
`app.elf` SHA256 is
`b50b4dea0013b57616373e2897cf4cb7d1f1113047ac1816e72b22816bb11e42`;
the disc ISO SHA256 is
`8c9fb463f1afc2aa18656165305820dd45dc5902679f578176593f1c5db73b72`.

The isolated ordinary PCE retail campaign passes through stage 7; see
`build/pce-regression/campaign-verification.json`. Ordinary PCE boss-pass,
sprite-priority and foreground checks also pass. Existing WIP corner,
camera-light, convoy, HUD-shake and sprite-overflow fixture failures also
reproduce on the preserved PCE image.
The apparent title blank capture was an uninitialized-RAM match during BIOS
startup: after requiring the application's initialized `SRPC` metrics,
fresh and preserved PCE title captures are pixel-identical (50 colors).
The presentation test now requires that readiness handshake. Its older
music-volume expectation still differs from the current option behavior on
both images. The diagnostic planar test also needs separate investigation;
no claim that the entire legacy PCE test suite passes is made here. The
consolidated comparison, exact failures and hashes are recorded in
`build/pce-regression/ordinary-pce-regression.md` and its JSON companion.

The full SGX campaign scenario suite passes on the final candidate: town,
race, platform bosses, mech, cruiser power and damage, ending, game-over,
return to title and hero-power cases. Essential sprite overflow, forbidden
reads and SGX runtime failures remain zero. See the current
`build/sgx/campaign-verification.json` and
`build/sgx/sgx-final-build-herd-report.md`.

Verification found and repaired an additional stage-7 dialogue allocation
regression before this final run. The captured failing state is preserved in
`/tmp/sgx-stage7-regression/first-overflow.json` with its native cache tables,
SATs and screenshot; `failurecheckpoint.md` explains the six-page admission
failure. The pre-fix SGX binary passed that stage-7 checkpoint (and later
failed a separate hero-power fixture). In the final campaign, dialogue ID 32
successfully uses a lower block at `$6700`; that run alone does not prove
admission of the unused tail block.

`python3 tools/pce/test_sgx_cache_pressure.py --out build/sgx` verifies that
fallback by seeding the captured failure's cache state and invoking the real
banked allocator on the native CPU. The frozen failing binary refuses it
(`cache_result=48`); the final binary allocates slot 8 at `$7800`, assigns all
six tail pages to that slot, and leaves protected pages 0–47 unchanged. The
minimal captured-state fixture is stored under
`tools/pce/fixtures/sgx_stage7_cache_pressure.json`; the result is
`build/sgx/sgx-cache-pressure-verification.json`. No host-side allocator model
is used. A final rebuild after test integration passes and produces exactly
the same ELF and ISO hashes above, so all final emulator evidence applies to
the delivered disc.

The starting SGX image is preserved at `/tmp/sgx-baseline-20261008`. The motion
report and startup screenshot are `build/sgx/motion-verification.json` and
`build/sgx/motion-startup-stage1.png`. Emulator fixtures and physical hardware
testing are separate evidence; no physical hardware testing is claimed.
