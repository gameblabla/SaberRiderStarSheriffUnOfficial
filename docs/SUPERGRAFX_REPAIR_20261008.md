# SuperGrafx repair, 2026-10-08

Follow-up to `codex-session-01a11936-93c9-79d3-a00f-fa0ef9593a9d.md`.

## Root causes and repairs

- **Power cut-in stopped gameplay:** HUD copy operands lived in switchable bank 114. Callers patched the same logical addresses while a different overlay was mapped, leaving the previous SAT transfer opcode and destination intact. HUD data then reached VDC0's control register and disabled VBlank. The copy loop and every patched operand now live in resident bank 106. Linker assertions enforce their placement. The per-frame display-control repair workaround is removed.
- **Shared sprite admission called the wrong overlay:** reserve/release routines called directly from multiple overlays now live in resident bank 106, with linker assertions.
- **Missing or corrupted platform bosses:** gameplay draws their hull on VDC1, but only VDC0 received its patterns. Hull changes now upload both VRAMs. VDC1's old sprite table is hidden during replacement and restored by the next paired publication; the original VDC0 replacement fence remains.
- **HUD and hero disappeared under boss/cache pressure:** platform cache blocks now protect two HUD generations, three hero pose generations, and three counter generations. Platform play can use the six spare pages through `$7dff`, stopping before the alternate SAT at `$7e00`. The arena retains its dedicated HUD region and its existing cache limits. Asset assertions reject oversized HUD/hero art or changed presentation layouts.
- **Foreground affected essential admission and dialogue:** SGX prepares scenery after the HUD/hero. Dialogue republishes its VDC1 foreground; the VDC0 overlap pass starts after the portrait and panel sprites. SGX retains both complete source foreground layers; optional runtime admission still observes actual hardware limits.
- **Invisible enemy projectiles could hurt:** platform projectiles retry against the second VDC's independent sprite budget. An enemy projectile refused on screen is retired and applies spawn pressure.
- **Clear dialogue repeated:** the final flying-boss defeat clears `boss_kind` when it queues the clear story, so subsequent gameplay ticks do not reopen it.
- **Arena exit hung or reset:** arena image A replaces bank 118, which owns the loading card. `change_stage` now restores that bank from the application image before calling the card. The restoration executes in untouched bank 113, masks raster callbacks while MPR6 holds the transfer bank, and restores MPR6 before callbacks resume. The scene loader subsequently restores the remaining temporary overlays as before.
- **Stage 7 showed only its background:** the SGX actor pass in bank 135 uses the `$c000` execution window. It now enters through an MPR6 wrapper instead of the MPR3 overlay trampoline. Its sprite upload helper executes in bank 116 and temporarily restores staging bank 108 for descriptors and scratch copies before returning to bank 135.
- **Cruiser greeting hung:** the general dialogue panel body lived in bank 115, but its wrapper selected bank 116. The wrapper now selects bank 115; build checks enforce its bank along with the new loader and space-rendering helpers.
- Direct VDC0 panel/cut-in writers restore the software index shadow alongside the hardware index and BIOS shadow.
- **Cruiser defeat jumped to address zero:** Stage 6's BG reset wrote arena cache flags through `$c000` stage-data addresses after MPR6 selected code bank 135. Six cleared bytes damaged a jump in the cruiser's destruction countdown. The reset now clears arena state before selecting the code bank. The compositor also maps stage data through MPR3 at `$6000` while executing through MPR6, translates its arena/scratch pointers to that window, and restores MPR3 on return. Its palette wrapper temporarily restores the normal stage-data window before the audio overlay consumes the palette pointer. Build checks guard both wrappers' bank placement.

## Regression entry points

```sh
make -f Makefile.pce test-sgx
make -f Makefile.pce OUT=build/pce all
python3 tools/pce/test_campaign.py --out build/pce
```

The campaign harness uses the retail frontend and native stage transitions. Seeds shorten encounters by changing positions, timers, projectile pools, and boss HP. Native CPU code performs damage, story paging, victories, game over, and power effects. Dark April's seed waits for her actual introduction/fight and targets her own Body.

The SGX harness checks hardware sprite budgets, essential admission, forbidden reads, and SGX transfer failures. It also compares platform hull patterns in both VDCs with the current stage archive. Essential admission failures report the refused sprite ID.

The follow-up harness compares every linked byte in code bank 135 with physical CD RAM after an active arena BG mech, arena exit, and the cruiser power. The BG-mech fixture keeps the target alive until its native compositor publishes a frame, then uses a separate one-hit target for the final wave. Input fixtures wait for stage readiness when the frontend has closed but loading is still in progress.

Build memory reports include the platform cache layout. Both release builds enforce bank bounds, console-memory bounds, unresolved-symbol checks, and the resident assembly assertions.

## Evidence and limits

Generated discs are `build/sgx/saber_rider.cue` / `.iso` and `build/pce/saber_rider.cue` / `.iso`. Verification records and captures are written beside each disc by the campaign harness.

The focused Stage 4 scenario exercised boss arrival, a health HUD change, hull destruction, the surviving sniper, and the final clear story with zero essential refusals. The repaired capture displays the complete green Hyperjumper above the source tower and foreground.

The cruiser-defeat continuation passed the complete seeded campaign harness on both the retail SGX and regular PCE images, including the ending, restart, game over, and all four hero powers. SGX code bank 135 remained byte-identical to its linked image after a visible arena BG mech, arena exit, and the cruiser power. SGX transfer failures and essential sprite refusals were zero. Results are `build/sgx/campaign-verification.json` and `build/pce/campaign-verification.json`; the separate stage-6-start follow-up is `build/sgx/final-stage-verification.json`. The broader `make test` suite was not run in this continuation.

These checks use the supplied accurate-core emulator, Arcade Card enabled, and hardware sprite limits enabled. They are seeded scenario coverage; physical hardware and a complete unassisted playthrough remain unverified. Optional scenery/effects can still be refused when the hardware/cache budgets fill. The broader SGX enhancement plan still includes menu pairing and a general arena software-object compositor.
