# Native PC Engine Arcade CD-ROM² port

The LLVM-MOS implementation is in `src/platform/pce/`; the host asset and disc
tools are in `tools/pce/`. The default boot enters the campaign. The disc image
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
regenerated from the current pack data and native source exports. Backgrounds
are baked together. Sprites store one facing and use the VDC flip bit.

Tests use `../PCE/mednafen-pce-headless`, the accurate PCE core with Arcade Card
enabled and hardware sprite limits enabled, and
`../PCE/[BIOS] Super CD-ROM System (Japan) (En) (v3.0).pce`.
`make -f Makefile.pce test-campaign` runs only the campaign scenarios.

## Controls

| Context | Controls |
| --- | --- |
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
and foreground masking `$74`. The resident trampoline preserves the previous
mapping across nested callbacks. Work and staging use `$6b/$6c`. CD transfers
use `$75–$7c`, copying through MPR6 into Arcade RAM before music starts.

The final 128 KiB of Arcade RAM is reserved for the background tile directory.
Scenes preload their graphics, collision, triggers, dialogs and mission tables.
CD-DA and the selected hero's hardware ADPCM bank coexist with PSG effects.
The runtime counts and rejects asset reads while music is active.

The ordinary VDC renders baked backgrounds and bounded sprite caches. The
allocator admits complete objects against 64 SAT entries and 16 width units
per scanline. Foreground alpha masks cut sprite pattern copies; cockpit slices
are clipped to the viewing window. Optional enemies that cannot be drawn are
removed, and hidden Ramrod mechs cannot inflict damage.

Both race phases use the Wolf BAT pair-character floor and raster scrolling at
the 512-dot clock. Horizontally expanded cars preserve their physical aspect.
Eight sampled depth rows repeat across 24 BAT rows, covering the 96-line floor.
The finished floor page is published after the corresponding sprite list is
prepared. Ramrod uses four baked sizes and overlapping 16-pixel slices.

## Verification and remaining work

On 2026-10-03 the hardware regression suite, campaign scenario suite, and native
foreground pattern checks passed on the supplied accurate-core emulator. The
eight-depth-row floor committed 150 pages in 600 video frames (about 15 Hz),
with hardware sprite limits enabled. The foreground comparison checked 12
patterns in two facings, including eight partially masked patterns. No essential
sprite overflow or forbidden asset read was recorded by those checks.

The renderer bank currently has 90 bytes free and the platform bank 321 bytes;
further code growth needs another overlay or relocation. These are actual link
limits, recorded in `runtime.json`.

`build/pce/verification.json` records asset/disc integrity, boot/rejection paths,
all four Arcade ports, collision/movement, hardware flipping, ADPCM data,
CD-DA output, floor refresh, cockpit clipping, reloads and checkpoint recovery.
`build/pce/campaign-verification.json` records campaign scenarios and captures.
Campaign tests explicitly seed long encounters with debugger writes to position,
timers, entity pools and boss HP. Native CPU code still executes hits, deaths,
dialog paging and transitions. These tests do not establish an unassisted full
playthrough or physical-disc timing.

`python3 tools/pce/test_foreground.py --out build/pce` compares actual VRAM
sprite patterns against the exported foreground alpha mask in both facings.
Its report is `build/pce/foreground-verification.json`.

The race currently follows the exported circuit with lateral steering and
coarse rival progress, rather than reproducing the source's free-world vehicle
simulation. Pursuit attacks and platform boss patterns are simplified. Ramrod
uses coarse range/aim combat and a baked floor. The space timeline retains its
combat events with smaller pools; radio messages, several original attack
patterns and decorative effects still need fuller adaptation. Presentation
uses text and still sprites; the full options UI, movie replacement sequences
and six-button mappings remain to be implemented.

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
