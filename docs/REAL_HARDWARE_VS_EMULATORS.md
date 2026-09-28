# Real hardware vs. emulators: the issues found on consoles

These bugs showed up on real consoles but not in the emulators we test with. Each entry lists what the console
showed, what the emulators do instead, the rule from the official documentation where one exists, our own
finding, and the fix.

- **Saturn:** Fenrir ODE on a retail console. No debugger, only the DIAG disc (`make -f Makefile.saturn DIAG=1`)
  and photos or video of the screen.
- **Saturn emulators:** Mednafen (headless kit) and Ymir. Ymir can run the SH-2 cache and the real CD-block firmware
  (LLE, `cdb106.bin`).
- **Dreamcast:** real console against flycast.
- **Sega documents** (copies with `.txt` extractions in `../Saturn/docs/`):
  - `ST-058-R2-060194`: VDP2 User's Manual
  - `ST-013-R3-061694`: VDP1 User's Manual
  - `ST-097-R5-072694`: SCU User's Manual
  - `ST-210-110194`: SCU precautions
  - `Sattechs`: the technical bulletins, including SOA #6, "VDP2 Cycle Pattern Registers", 16 Aug 1995

Status as of 2026-09-28 is given per item. "Confirmed" means the fix was checked on the console.

---

## Saturn

### 1. SMPC INTBACK never ended: black screen right after the SEGA logo

- **On the console:** the screen went black straight after the BIOS logo, with nothing more.
- **Emulators:** Mednafen booted normally through the logo, the load pause and the intro FMV.
- **Cause:** a pad-reading INTBACK is issued every vblank. When the previous answer had not been read yet (on the
  2nd vblank of boot, before the main loop runs), the handler returned without sending CONTINUE or BREAK. That left
  the SMPC status flag (SF) set. The next SMPC command, SSHON in `rsat_init` with interrupts off, then spun
  forever waiting for SF to clear.
- **Rule:** the SMPC manual's INTBACK procedure requires every INTBACK that asks for peripheral data to be ended
  with a CONTINUE or BREAK through IREG0. An SMPC command must not be issued while SF is set.
- **Emulator difference:** Mednafen drops an unfinished INTBACK at the next vblank and clears SF
  (`smpc.cpp`, `JR_WAIT ... PendingVB`). The real SMPC does not.
- **Fix** (`smpc_peripheral_sat.c`):
  - The INTBACK is always ended, with a BREAK when its answer was unread or overflowed.
  - A new INTBACK is only issued once the last one was read (`sat_smpc_intback_ok`).
  - The game's own SMPC commands (slave CPU and sound CPU on/off, the clock change) go through
    `sat_smpc_lock`/`unlock`, which waits out the INTBACK in flight and BREAKs it if it hangs.
- **Also hardened in the same pass:** the slave SH-2 handshake. The master re-notifies the slave when an ICI is
  lost. If the slave never answers within 1 s, the master stops it with SSHOFF and draws alone.
- **Status:** the console then got past the black screen and reached the white screen of item 3. The black screen
  of the normal (non-DIAG) build was most likely the CD hang of item 3 as well. The DIAG text could not show then
  because of items 5 and 6.

### 2. Peripherals other than a pad crashed libyaul

- **Found:** in Mednafen with its keyboard. It applies to real mice, keyboards and MD pads too.
- **Cause:** libyaul's `_peripheral_update` hits `assert(false)` on a peripheral whose data is not 2, 4 or 6 bytes
  long. Examples: Saturn mouse 0x23, MD mouse or keyboard 0xE3, MD 3-button pad 0xE1.
- **Fix:** a game-local `smpc_peripheral_sat.c` that accepts any size and reads only pad-shaped devices.

### 3. The CD block answers WAIT: white screen, "cannot open" packs

- **On the console:** DIAG boot stage 7 (white) hung while loading the game's packs.
- **Emulators:** Mednafen and Ymir's HLE CD block never answer WAIT. Every command is accepted at once.
- **Reproduced by:** Ymir with **CD-block LLE**, which runs the real firmware `cdb106.bin`. 
  It logged `pack: cannot open /video.pck` and `app_init failed`, exactly like the console.
  This is the only emulator setup that has reproduced a console CD bug so far.
- **Firmware behaviour (our finding):**
  - A command the CD block cannot take yet comes back with **WAIT** (status byte bit 7) and is not run.
  - That happens to a selector command sent before the previous selector operation finished (before ESEL), and
    while the drive seeks.
  - libyaul treats that answer as a success.
- **Fix** (`cd_sat.c`):
  - `cd_exec` resends a command answered with WAIT, for up to 2 s, and logs if it gives up.
  - Selector commands (filters, selector reset, device connection) go through `sel_cmd`, which also waits for ESEL.
- **Status:** confirmed. The console got past boot and into the game.

### 4. CD block command latency starved the FMV

- **On the console:** the intro FMV stalled.
- **Emulators:** the HLE CD blocks execute every command instantly.
- **Cause:** on the real firmware each command takes **milliseconds**. We read one sector per request, which gave
  only ~60 sectors a second. The Cinepak intro needs ~88.
- **Fix:** a 16-sector read-ahead (`RA_SECTORS`) filled with whatever the CD block already holds, one command
  sequence per batch.
- **Also fixed here:** data is only read from the port once **DRDY** is set. Before, EHST alone ("command over", no
  transfer) was also taken as "data ready". The words read then were not the sectors: the intro's garbage stream on
  the console, with the decoder writing outside its picture.
  - HIRQ `EHST` is `0x0080`. `0x0400` is `SCDQ`, set on every subcode update while CD-DA plays, and it ended the
    DRDY wait early.
  - The transfer's word count from End Data Transfer must equal the sectors asked for. A short count fails the read.
- **Status:** confirmed. The intro plays on the console.

### 5. Colour RAM written during active display is lost

- **On the console:**
  - Level 1's far plane (NBG0) was black where its palettes should be.
  - Single colours in other palettes were black or stale (dashes across the rocks).
  - Stage 2's floor was grey in the distance.
  - The DIAG font showed vertical stripes.
- **Emulators:** Mednafen and Ymir accept a CPU write to CRAM at any time.
- **Rule:** VDP2 manual ST-058-R2, "Color RAM": *"Read/write access from the CPU or DMA controller is possible,
  but the image may be disturbed by the access timing."* Access is by word or long word only; bytes are not allowed.
- **Finding:** on the console, writes made while VDP2 draws the screen appeared to be lost, not only disturbed.
- **Fix** (`render_sat.c`, `main_sat.c`):
  - Every CRAM writer puts its colours in an **uncached copy** (`cram_copy`) and marks the 64-entry granules it
    touched (`cram_dirty`). The writers are the planes, the floor and fog, and the slave's 8bpp sprite banks.
  - The vblank-in handler (`rsat_cram_flush`) copies only the marked granules to CRAM.
  - The DIAG overlay writes its font, palettes and back screen from vblank-in only.
- **Status:** confirmed together with item 6. This fix alone did **not** fix level 1.

### 6. VDP2 VRAM cycle patterns: the console's rules

- **On the console:**
  - Level 1's NBG0 stayed black.
  - "Cheese holes": 8×8 cells that were black or showed the wrong tile, moving with the rocks. These are lost
    name-table writes.
  - The DIAG overlay was striped.
- **Emulators:** Mednafen and Ymir accept any cycle pattern and serve CPU and VDP2 reads from any bank at any slot.
- **Rules:**
  - **ST-058-R2 §3.3, "Read/Write Access by the CPU":** during display, the CPU only gets VRAM at the timings its
    cycle pattern registers give it. *"VRAM access by the CPU can be selected only in units of access to VRAM-A or
    VRAM-B, and can not be selected in bank units."*
  - **Bulletin SOA #6:**
    - *"CPU access for bank A0 should match the CPU access for bank A1, and CPU access for bank B0 should match the
      access for bank B1. Cycles which are available for CPU access in, say, bank A0 but not in bank A1 should be
      programmed with access code 0xf (no access). If all of bank A … or all of bank B is unused, set its cycle
      pattern registers to 0xfeeeeeee."*
    - It also gives the table of the slots where character data may be read relative to its pattern-name read, and
      at most two pattern-name reads per slot.
  - **Ymir's `docs/dev-notes/system-info/vdp2-vram-access-cycles.txt`** covers the same PN/CP placement rules.
- **What we broke:**
  - The CPU had slots T4–T7 right after a name or character read.
  - A CPU slot in one half of a chip sat across a read in the other half. For example, NBG0 read characters in A0
    at T0 while A1 had a CPU slot there.
- **Fix** (`vdp2_planes.c` `set_cycle_patterns`, `set_upload_patterns`):
  - While a level uploads (nothing shown): `0xFEEEEEEE` in every bank.
  - While a level plays:
    - T0–T3 hold NBGn's reads at Tn: names in B1, characters in each bank that holds the plane's cells, 0xF
      otherwise.
    - T4 is 0xF, the "no access" before the run of CPU slots.
    - T5–T7 are CPU.
  - The new patterns go straight into the registers (`put_cycle_patterns`) **before** a bulk VRAM upload.
    libyaul's shadow copy only reaches the hardware at the next commit, and its default is all 0xF: no CPU slot.
  - New name-table columns and the NBG0/1 line-scroll tables are queued during the frame and written from vblank-in
    (`sat_planes_shown`). The CPU can use VRAM freely in the blank (SOA #6).
  - The DIAG overlay follows the same rules.
- **Status:** confirmed. Levels 1, 2 and 3 render correctly on the console (commit 4d40fba).

### 7. A plane reads characters only from banks where it has a CP slot: level 4/5 purple band

- **On the console:**
  - Level 4 showed flat purple over its top 12 rows. It covered the sky and the tree canopy, but not the palm
    trees, which are on a plane further back.
  - Level 5 has the same layout (its front plane's cells are in B0, with 16 empty top rows) and is expected to show
    the same band.
- **Emulators:** Mednafen and Ymir fetch a character from any bank whatever the cycle patterns say. No emulator
  showed the bug. Mednafen's debugger `mem_write vdp2vram` does not reach its renderer either, so poking VRAM cannot
  test this. A `SABER_*` switch built into the program can.
- **Rule:** ST-058-R2 §3.3, under the cycle pattern register figure: *"If the VRAM access address selected in the
  VRAM cycle pattern register is not the address in the selected bank, access won't be done and the correct screen
  will not be displayed."*
- **Cause:**
  - Rows outside every scrolling band used pattern name 0, global cell 0. That is NBG0's blank cell, in bank A0.
  - Level 4's NBG2 has all its cells in A1, so it has character slots in A1 only.
  - Its empty rows therefore asked for a character in a bank it has no slot in, and drew garbage: flat purple.
  - Levels 1–3 were only safe because their planes with empty rows keep their cells in A0.
- **Fix** (`vdp2_planes.c` `empty_name`): an empty cell is the plane's **own** first cell (`layers.py` always makes
  it blank). Every character read then stays in a bank where the plane has a slot.
- **Rule of thumb:** every name a plane shows, including "transparent" filler, must point at characters in banks
  that plane's CP slots cover.
- **Status:** fixed in commit 5d83422. Awaiting the console check with the DIAG disc: OPTIONS > STAGE 4 and 5.

### 8. A disc read during a CD-DA track start: the title never reached OPTIONS

- **On the console:** the title screen hung when going into OPTIONS.
- **Emulators:** neither Mednafen nor Ymir (even with CD-block LLE) reproduced it.
- **Cause:**
  - A data read stops CD-DA on the Saturn.
  - OPTIONS started its music track while the drive was still seeking to it, then read its art on the next frame.
  - That read took the drive back from the track. A failed art load was retried every frame, so one failing read
    became a permanent hang.
  - Every other menu already did its reads first.
- **Fix** (`menu.c`, `MS_OPTIONS`): the options art is read before its music starts. Menu transitions go to the
  Saturn log.
- **Status:** not reported again since the fix. The console gets into OPTIONS.

### 9. A stale sector taken for the one asked: stage 5 refused after stage 4

- This one was also visible in Mednafen from the user's savestate. It is listed because it comes from real drive
  behaviour.
- **Cause:**
  - The stream filter passes the whole file's FAD range, so sectors the drive was still reading from its previous
    position in the same file passed it too.
  - Stage 5's sound bank got `SND.PCK` sector 203 in place of sector 80.
  - Sample `E105C92A` failed its ADPK check and the game went back to the title.
- **Fix:** `read_sectors` checks each batch's FAD with Get Sector Info and drops stale sectors.
- **Related libyaul bug:** libyaul's `cd_block_cmd_*` check the response's low byte against the status codes. That
  byte holds the CD-DA repeat count and flags. Once a looping track had repeated an odd number of times, every
  command "failed": the boss music never started and the victory screen went blank.
  - Fix: our own `cd_cmd_st`, which reads the status from the high byte.

### 10. Open: a very slow stage load after switching to 352x224

- **On the console:** after OPTIONS > SCREEN 352x224 (and more lives / continues), starting the game took an
  absurdly long time to load, with what looked like retries, before the stage began. Default settings load normally.
- **Emulators:** not reproduced. Ymir with CD-block LLE, same menu steps, loads stage 1 in about the same time at 320
  and 352. Mednafen reads the same packs in the same order in both modes, with no CD errors.
- **What the clock change does** (BIOS `SYS_CHGSYSCK`, 0x06000320 -> ROM 0x4C8 and 0x1800, disassembled): SMPC
  RESDISA, the master SH-2 into standby, CKCHG352 / CKCHG320, wake on NMI, then the SCU is set up again with the same
  A-bus timing (ASR0 = ASR1 = 0x1FF01FF0, AREF = 0x1F), and RESENAB. The CD block's bus settings survive it.
- **Next step:** the DIAG disc's stall screen now shows the log during a load (see below), and it logs every pack read
  with its time plus one summary per stage load (`load: stage N in X ms`, then `cd: ... reads, seeks, WAIT answers,
  stale sectors, failed reads`). A photo of it during the slow load says where the time goes.

### Debugging method that worked

- **DIAG disc** (`DIAG=1`):
  - The log ring shows on screen at boot and on any main-loop stall longer than 4 s.
  - Boot stages show as full-screen VDP2 back-screen colours, which need no tiles, font or VRAM timing:
    - blue: patched IP.BIN (`tools/saturn/diag_ip.py`)
    - then red, orange, yellow, green, cyan and white for `sat_diag_stage(2..7)`
  - Its font is written again each time it comes up. Written once at boot, the menus' and stages' planes had
    overwritten it by the first level load: the stall overlay showed vertical stripes on the console and nothing in
    Ymir.
  - It logs every pack read with its time (`SABER_READLOG` on by default) and a summary per stage load.
- **OPTIONS > STAGE on the DIAG disc:** starts any stage directly as Fireball.
- **Ymir with CD-block LLE:** use it for **any** CD-block change before a hardware run.
- **Emulators can't check VDP2 timing:** they are not a reference for cycle patterns, CRAM timing or bank access.
  Check every such change against ST-058-R2 §3.3 and SOA #6 by hand.

---

## Dreamcast

### 11. 640×480 / 832×480 at 20–30 fps: PVR fill rate

- **On the console:** the high-resolution modes ran at 20–30 fps.
- **Emulators:** flycast always showed 60 fps. It renders the PVR scene on the host GPU and cannot show the PVR's
  render time.
- **Wrong first fix:** `-fno-unroll-loops` (sh4zam#69), which addresses a CPU-side GCC issue. The slowdown was
  never the CPU.
- **Cause:**
  - Every draw went to the **translucent** list with autosort off. The translucent list shades every pixel of
    every layer.
  - Stage 1 is ~2.9 screens of layers, and 640×480 has four times the pixels of 320×240.
  - A 17–40 ms render is shown at the next vblank KOS gets, which gives 30 or 20 fps.
- **Fix** (`render_pvr.c`, commit 61e8aa8):
  - Each primitive gets its own depth, rising in submission order, so the painter's order holds.
  - Each draw goes to the cheapest list that gives the same pixels:
    - opaque, where only the front pixel is shaded
    - punch-through, for 1-bit cut-outs drawn nearest
    - translucent, only for real blending
  - A per-8×8 alpha map of each texture classifies the exact source rectangle, so a tile sheet's solid tiles go
    opaque.
  - Stage 1 at 640×480 went from 2.87 translucent screens to 1.76 opaque + 0.93 punch-through + 0.17 translucent.
- **Status:** not yet measured on the console. Use `SABER_PERF=2` for per-list loads, and `SABER_PVR_TR=1` for the
  old path as an A/B comparison.
