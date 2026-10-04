# Native PC Engine Arcade CD-ROM² port

The LLVM-MOS implementation is in `src/platform/pce/`; the host asset and disc
tools are in `tools/pce/`. Boot opens the 320x224 title, then options or four-hero
selection (the briefing was cut). Select on the title, or the Continue item,
resumes an existing checkpoint. The disc image
is `build/pce/saber_rider.cue` and must be kept beside its ISO and music files.

This is a playable, bounded campaign adaptation. It includes the seven internal
stage scenes, race-to-pursuit transition, platform bosses, Dark April, three
Ramrod waves, the space cruiser, dialogs, powers, lives and ending/game-over
states. It is not yet acceptance of every gate in `PCE_ARCADE_CDROM2_PLAN.md`.

## Build and run

From `game/`:

```sh
make -f Makefile.pce all
make -f Makefile.pce test
```

`MOS` defaults to `../PCE/llvm-mos8`. Compiler, headers, linker scripts and
`pce-mkcd` must come from that same SDK. The recorded compiler revision, bank
sizes and VRAM layout are in `build/pce/runtime.json`. The build rejects missing
application symbols, floating-point/64-bit arithmetic helpers, overflowing code
banks, and console state that overlaps the reserved software stack.

The host pipeline requires the existing headless game build, a C compiler,
Python with NumPy/Pillow, and ffmpeg for audio conversion. Stage archives are
regenerated from the current pack data and native source exports. Background layers
are baked together; foreground occluders are separate hardware sprites. Sprites store one facing and use the VDC flip bit.

Tests use `../PCE/mednafen-pce-headless`, the accurate PCE core with Arcade Card
enabled and hardware sprite limits enabled, and
`../PCE/[BIOS] Super CD-ROM System (Japan) (En) (v3.0).pce`.
`make -f Makefile.pce test-campaign` runs only the campaign scenarios.

## Controls

| Context | Controls |
| --- | --- |
| Title / options / selection | Up/down choose; left/right change an option or hero; Run or I/II accept; Select on title resumes the checkpoint |
| Platform | Left/right move; II jump; down crouch; down + II drop through a one-way platform or slide on solid ground; I shoot |
| Aim / power | Hold Select with directions to aim; press Select while holding I for a power |
| Race / pursuit | Up gas, down brake, left/right steer, II turbo/ram, I fire in pursuit |
| Ramrod | Left/right aim, up/down change range, I guns, II close punch |
| Space | Directions fly, Select slower movement, I shoot, II torpedo, Select + I hero power |
| Dialog / clear | I or II advance |
| Run menu | Left/right stage, up/down hero, II campaign/diagnostics, Run resume/start, Select planar effect demo |

Resuming unchanged menu choices keeps the current mission and avoids a disc
reload. Changing stage, hero or mode reloads the selected scene. A paused dialog
currently resumes at its first page. The BIOS BRAM checkpoint stores stage,
hero, and pursuit phase with a version and checksum; it does not store a whole
mid-stage game. Lives carry across normal stage transitions. Platform and space
powers have two charges, with separate cooldowns.

## Runtime layout

The resident kernel and IRQs remain in bank `$68`; the renderer stays in `$6a`.
Stage callbacks share the `$6000` CPU window: platform `$69`, floor `$6d`, flow
`$6e`, race `$6f`, platform combat `$70`, story `$71`, Ramrod `$72`, space `$73`,
and presentation/sprite allocation `$74`. The resident trampoline preserves the previous
mapping across nested callbacks. Work and staging use `$6b/$6c`. CD transfers
use `$76–$7c` (56 KiB), copying through MPR6 into Arcade RAM before music starts.
Audio controls and shot/power DAC bytes occupy `$75`; impact/gallop DAC bytes
occupy `$7d–$7f`.
The unrolled playback service uses the always-mapped `$6b` work bank.
The timer IRQ saves and restores MPR6 while leaving MPR3 and X/Y unchanged.
The herd renderer passes strict 60 Hz presentation checks for all three
stampedes with continuous firing; see [PCE_HERD_60FPS.md](PCE_HERD_60FPS.md).

The final 128 KiB of Arcade RAM is reserved for the background tile directory.
Scenes preload their graphics, collision, triggers, dialogs and mission tables.
CD-DA and the selected hero's hardware ADPCM bank coexist with two timer-driven
Build 14 DDA voices decoded during asset generation, on PSG channels 0–1 at
approximately 6.99 kHz: gunfire/impact/power share channel 0; the looping horse gallop uses
channel 1. A new one-shot effect replaces the previous one-shot. The asset builder uses
the supplied ROM’s exact predictor and adaptation tables to generate identical
5-bit DAC bytes. The IRQ delivers them directly to each PSG channel, allowing a
one-shot effect to overlap the gallop with less CPU work.
Jump, hurt and death use ADPCM voices without
added tones. A death voice finishes before reload; blocking loaders stop the
DDA timer and every PSG channel.
The runtime counts and rejects asset reads while music is active.

The ordinary VDC renders baked backgrounds and bounded sprite caches.
Platform backgrounds reserve palette 15 for text/dialogue and 32 characters
at `$4000–$41ff` for panel tiles, leaving 896 background cache characters. The
allocator admits complete objects against 64 SAT entries and 16 width units
per scanline. HUD digits/icons precede the foreground sprites in SAT priority, followed by
actors/projectiles. Foreground art shares one sprite palette and a separate
cache-ID range; actor patterns remain intact. Cockpit slices are clipped to
the viewing window. Platform BYR follows the stampede’s 0–3 pixel world shake; the HUD stays
fixed. Only the race uses raster scrolling. Level 1 has a blue sky gradient, and level 3 a dark purple one.
Both moons are removed. Optional enemies that cannot be drawn are
removed, and hidden Ramrod mechs cannot inflict damage.

Both race phases use the Wolf BAT pair-character floor and raster scrolling at
the 512-dot clock. Horizontally expanded cars preserve their physical aspect.
The floor begins at scanline 120 (the sky above it is plain gradient tiles): seven far strips of 8 scanlines (half-resolution samples) and twelve near strips of 4 make 19 BAT rows.
The finished floor page is published after the corresponding sprite list is
prepared. Ramrod uses four baked sizes and overlapping 16-pixel slices.

## Front end

`tools/pce/frontend.py` bakes three 320x224 screens (40x28 BG characters, VCE
7.16 MHz clock): title, hero select, and an options/continue/credits panel.
Runtime code is `ui_pce.c` (renderer bank, shared services), `frontend_pce.c`
(title/options/select) and `credits_pce.c` (continue, game over, credits).

- Colour limits are avoided with sprites, which have their own palettes: the
  chosen hero's portrait is 16x16 sprite pieces with a free palette each; the
  title logo's worst-quantized blocks are covered by sprite patches; menu items
  and hero names are sprites too.
- Animation is palette driven: the tunnel cycles four palettes; hero panels
  switch dim/selected/glow palettes; menu highlights swap sprite palettes.
- Text is BG font characters on a flat panel colour, so there are no black boxes.
- Options live in `pce_options` (difficulty, lives, continues, music). Hearts are
  3/2/1 and enemy fire intervals 120/90/62 frames. Lives and continues are capped
  at 7/5, 5/4, 3/3 by difficulty. The CD fader can only ramp to silence, so
  music volume selects one of three attenuated copies of every track (blocks at
  tracks 2, 21 and 40) or turns music off; the disc carries all three.
- `emulator-profile.patch` and `tools/pce/profile.py` add a per-function
  CPU profile to the headless emulator.

## Rendering speed

Platform stages run at 60 Hz. The draw path uses block-move Arcade reads
(`TAI`), a resident cache that skips the Arcade lookup for cached sprites,
an assembly sprite emitter, retained foreground parts (`foreground_pce.c`),
exact scanline admission for the 16 width units per line limit, segment-wise
SAT upload, and a +64 address-increment BAT column writer. The race floor and Ramrod cockpit were not
sped up.

## Verification and remaining work

The baseline WIP commit is `9d04f6e`. The playtest list and implementation
status are in `PCE_ISSUES.md`. The supplied accurate core checks the real
VDC/PSG/ADPCM hardware paths with sprite limits enabled. Reports and review
artifacts are generated under `build/pce/`.

`runtime.json` records current bank headroom; the renderer remains close to
its 8 KiB limit. Add future presentation code to the `$74` overlay rather
than assuming the renderer has room.

`build/pce/verification.json` records asset/disc integrity, boot/rejection paths,
all four Arcade ports, collision/movement, hardware flipping, ADPCM data,
CD-DA output, floor refresh, cockpit clipping, reloads and checkpoint recovery.
`build/pce/campaign-verification.json` records campaign scenarios and captures.
Campaign tests explicitly seed long encounters with debugger writes to position,
timers, entity pools and boss HP. Native CPU code still executes hits, deaths,
dialog paging and transitions. These tests do not establish an unassisted full
playthrough or physical-disc timing.

`test_foreground.py` checks unmodified actor patterns and actual foreground
SAT priority in both facings. `test_presentation.py` drives the title, all
four selections, briefing, HUDs and sixteen diagonal poses through real input.
Its screenshots are in `presentation-review/`. `test_audio.py` calls compiled
native audio entry points in an isolated scenario and records emulator output
to `audio-review/*.wav`; it checks the sample clock, bank restoration, silence
at sample end, shutdown during an active shot, and all four heroes' voices.

The race currently follows the exported circuit with lateral steering and
coarse rival progress, rather than reproducing the source's free-world vehicle
simulation. Pursuit attacks and platform boss patterns are simplified. Ramrod
uses coarse range/aim combat and a baked floor. The space timeline retains its
combat events with smaller pools; radio messages, several original attack
patterns and decorative effects still need fuller adaptation. Platform dialogue
uses a top BG panel and four rounded sprite corners, with
original 2D speaker portraits. Closing restores its saved world columns, font
and palette from Arcade RAM; displayed sprite generations remain pinned through
SAT DMA.
The full options UI, later movie replacement sequences and six-button mappings
remain to be implemented. Dense scenes can drop optional sprites under the
hardware limits and still need visual playtesting.

Remaining acceptance work includes an unassisted campaign playthrough, dense
combat and foreground edge cases, simulation/render timing measurements, and
real PC Engine/Arcade Card tests. The current port should not be described as
fully matching the source game or as passing M7.

## Emulator automation extension

`PCE/mednafenPceDev-main/mednafen/src/drivers_libxxx/main.cpp` supplies the test
RPC commands `input`, `asread`, `aswrite`, `registers` and `sound_status` in
addition to the existing run/memory/screenshot channel. `aswrite` accepts a
debugger address-space name, address and hex bytes; campaign tests use it to
seed scenarios. No CPU timing or sprite-limit behavior is changed. The original
supplied binary remains at `PCE/mednafen-pce-headless.original`.
Rebuild through `PCE/mednafenPceDev-main/build_headless.sh`, then copy its
`mednafen/src/mednafen-pce-headless` output to `PCE/mednafen-pce-headless`.

## Supplied 2-bit software ADPCM example

`tools/pce/adpcm2.py` creates streams for the supplied build 14 ROM's actual
adaptive decoder. It packs low pairs first, uses the ROM's magnitude/index
lookup tables, saturates its 16-bit predictor, and writes sample-count/rate
metadata plus a decoded WAV preview:

```sh
python3 tools/pce/adpcm2.py build/pce/work/sfx/C66E1894.wav build/pce/shot-2bit.bin
```

`test_adpcm2.py` compares the host decoder and encoded streams with the ROM's
native HuC6280 routine in 48 seeded cases. This establishes format compatibility,
not the reported 17% CPU budget. The ROM consumes samples through a scanline
IRQ and two weighted DDA channels. The game expands two independent streams
to exact DAC bytes during the build and delivers them on the timer IRQ, keeping
the race raster scheduler separate. `test_software_adpcm.py` compares all 30,047
DAC bytes with the codec and checks native playback across bank and loop
boundaries; `test_audio.py` checks concurrent delivery, stopping and audible
native output. See `PCE_AUDIO_REFERENCE.md` for exact locations.

The frontend-only `tools/pce/emulator-audio.patch` adds `sound_capture` and
`register_set` RPC commands for these checks; it does not change emulated
hardware or timing. It is applied to the local headless emulator source and
binary. The previous binary is kept as `PCE/mednafen-pce-headless.pre-audio`.

The 2026-10-04 boss/presentation revision restores native boss sizes, two-frame
security cameras and all 19 victory paintings at 320×224, removes boss HP
readouts, resets rumble before dialogue, and shortens convoy spacing to 224 px.
Race profiling with seven rivals measures 61 road updates per 300 video frames,
up from 34. See `docs/PCE_BOSS_RACE_PRESENTATION_FIX_20261004.md` for the
implementation, measurements and emulator verification artifacts.

The second 2026-10-04 revision (boss far/mid-pass hulls and sounds, the jingle and the 320x224 victory painting with glowing
lettering, the blue-gradient race sky, the 19-strip floor and 15 road updates per second, stage title cards and the race controls card)
is described in `docs/PCE_BOSS_VICTORY_RACE_FIX_20261004B.md`.
