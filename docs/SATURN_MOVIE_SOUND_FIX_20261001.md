# Saturn movie sound handoff — 2026-10-01

Reported on a real Sega Saturn: skipping Saber's special movie leaves music
and game sound silent until another movie plays.

The Saturn backend held the MC68EC000 in reset throughout each movie.
`aud_movie_begin()` called `scsp_quiet()` (SNDOFF) and the next SNDON occurred
only when `aud_movie_end()` reloaded the ADPCM driver. This violates Sega's
sound CPU restrictions and leaves sound RAM/SCSP operation unguaranteed on
hardware. The emulators used here did not reproduce the reported persistent
silence; they did demonstrate the prohibited stopped-CPU interval.

## Sega references

- [Technical Bulletin #51, restrictions 1–2](https://antime.kapsi.fi/sega/files/ST-TECH-51.pdf):
  use SMPC for sound CPU reset; stop only to load its program, restart
  immediately, and run an endless loop when it has nothing to process.
- [SMPC User's Manual, SNDON/SNDOFF](https://www.infochunk.com/saturn/segahtml_en/hard/smpc/hon/p02_30.htm):
  the sound CPU must not remain stopped with a sound-RAM access gap of 0.5 s.
- [SCSP User's Manual, startup/reset vectors](https://www.infochunk.com/saturn/segahtml_en/hard/scsp/hon/p01_20.htm):
  initialize MEM4MB/DAC18B, install the SSP/PC reset vectors, then release
  sound CPU reset through SMPC.
- [SCSP User's Manual, interrupt control](https://www.infochunk.com/saturn/segahtml_en/hard/scsp/hon/p04_2c.htm):
  pending flags are available for polling without enabling interrupts;
  timer C is bit 8 and MCIRE clears the selected pending flags.

The local manuals in `../Saturn/docs/` and Mednafen's `src/ss/smpc.cpp`,
`sound.cpp`, and `scsp.inc` were also inspected. Mednafen implements SNDON as
a sound CPU reset/start, separate from SCSP reset.

## Changes

`src/platform/saturn/aud_sat.c` now installs a small idle program after
quiescing the SCSP: SSP = $80, PC = $8, and opcode $60FE (`BRA.S` to itself).
It issues SNDON immediately after uploading that program. The 68000 runs
throughout playback while the SH-2 programs the movie's PCM slots. Movie exit,
including a skip, still uploads the complete game sound driver and waits for
its startup acknowledgement. The resident sample table and bank are retained.

`third_party/libyaul_cinepak/film_snd.c` now polls timer C through explicit
16-bit accesses, writes only bit 8 to MCIRE, and reloads timer C with $0200.
It no longer enables an unused main-CPU sound interrupt. Inspection of the
generated SH-2 assembly confirms the pending register is read with `mov.w`;
the previous bitfield read used `mov.l` across adjacent SCSP registers.

## Validation

- `python3 tools/saturn/tests/run_resident_audio.py` passed. New cases check
  the running movie CPU, complete driver reload, retained bank/table,
  restored CD gain, and acknowledged SFX playback for streamed and RAM
  movies. They perform no disc reads. The same regression against the
  original backend fails on the running-CPU assertion.
- `python3 tools/saturn/tests/run_cd_loading.py` passed.
- `make -j8 -f Makefile.saturn` passed, including the no-soft-float check.
  The playable build is `build/saturn/saber_rider.cue` and `.bin`, with no
  diagnostic environment added to the release disc.
- Ymir captured 2,400 fields for original and fixed builds. During Saber's
  movie (sampled fields 1430–1490), the original CPU was disabled and MCIEB
  was $0100; the fixed CPU was enabled in its idle loop and MCIEB was zero.
  Both emulator runs restored game audio after the skip.
- Mednafen WAV captures checked Saber skip with the power button, natural
  completion, a second Saber movie skipped with pause, and Fireball skipped
  with pause. Post-movie windows had RMS levels from 2,670 to 4,915 PCM units;
  none stayed silent. The first-use cases passed the disc-read tripwire.
  The second-use case intentionally opens `SABER.CPK` again, so the generic
  tripwire flags that movie read. Its log contains only that expected read,
  and audio resumes after the second skip (RMS 3,957 then 4,663).

Artifacts, source revision/image hashes, environments, host test logs, WAV
checks, and Ymir CPU/audio measurements are in `artifacts/saturn-movie-audio/`.
These checks verify the corrected sequence in code and emulators. A real
Saturn retest is still needed to confirm the reported hardware symptom is
gone.

## Follow-up 2026-10-04: glitching at the first stage start (some NTSC-M consoles)

Reported by a tester on one NTSC-M console revision, not seen on another console or in Mednafen/Ymir: sound glitching,
"as if a buffer were not cleared", when the first stage starts after character select. The 2026-10-01 validation
above drove stages through SABER.ENV shortcuts and never ran the menu path (briefing clip -> character select ->
load -> stage 1); that path was run headless here and behaves the same as before in both emulators, so this change
is hardening against hardware-only state, **not a confirmed root-cause fix**. It needs a retest on the affected console.

What the code relied on that only an emulator guarantees:

- Sound RAM was never cleared: only the effect table was zeroed, so the rest held the console's power-on DRAM pattern
  (it differs between hardware revisions; emulators start at zero), and the bank was refilled over the previous scene.
  Anything a slot or the DSP reads outside a sample's own bytes was therefore undefined. Sega Technical Bulletin #36
  (Sattechs.txt): initialize every chip, never rely on boot-ROM / power-on state.
- `aud_movie_end` restarted the 68000 with `driver_start` alone, on top of whatever the clip had left in the SCSP
  (the film player's slots 0-1 and timer C, the CD pass-through on slots 16-17). Boot and `aud_movie_begin` both
  reset the chip first.

Changes (`src/platform/saturn/aud_sat.c`):

- `aud_init` zeroes all 512 KB of sound RAM (68000 off) before loading the driver.
- `driver_start(true)` (movie end, clock change) runs `scsp_quiet()` first: every slot, timer and DSP register cleared,
  as at boot.
- `aud_movie_end` zeroes the clip's ring (0x78000-0x7FFFF) before the driver restarts.
- `aud_prepare_scene` zeroes the effect table and the whole bank once every channel's stop is acknowledged, then
  loads the scene's samples.

`tools/saturn/tests/resident_audio.c` now poisons the ring, the table end and the bank end and asserts they are zero
after a movie end / scene prepare. Mednafen menu run (title -> briefing -> select -> stage 1): stage load time and
per-second audio levels match the build without the change.

## Follow-up 2026-10-04: release playback while holding the last frame

The briefing deliberately keeps its `Video` open while the player reads the
text. Previously, `video_sat.c` only marked `done` at FILM `END`/`ERROR`;
PCM slot key-off, decoder/stream cleanup and `aud_movie_end()` waited for
`video_close()`. The last sound ring could therefore keep looping throughout
the held image, despite the previous commit's ring clearing on close.

Playback completion now detaches the renderer's decode hook, drains/stops film
audio through `film_audio_reset()`, closes any data stream, restores the game
sound driver, and releases the RAM clip and decoder buffers. FILM `PAUSE` also
finishes playback as a held still image; the Saturn video API has no resumable
movie-pause operation. Power-attack buffers retain their existing spare-cache
policy so repeated powers still have decoder RAM available. The `Video` keeps
its dimensions and VDP1 surface for drawing until the screen closes it.
Closing this finished object performs no second audio reset or driver restart.
Early skips and discarded preloads use the same cleanup path.

The local VDP1 manual, `../Saturn/docs/ST-013-R3-061694.txt`, introduction
(lines 218–226, 263–264), distinguishes texture/command VRAM from the erased
framebuffer. Accordingly, the fix keeps the RGB555 movie texture reserved and
draws it each frame: it does not rely on framebuffer contents surviving erase
or swap. The existing movie sound handoff and running idle sound CPU remain in
use.

Validation:

- `python3 tools/saturn/tests/run_movie_lifecycle.py`: passed against the actual
  video backend with hardware/decoder stubs. Covers END/ERROR/PAUSE, RAM and
  streamed clips, buffer release/reuse, preserved pixels and dimensions,
  idempotent completion/close, early skip and discarded preload.
- `python3 tools/saturn/tests/movie_hold.py`: passed in Mednafen on the normal
  menu route (intro skip -> briefing -> wait at last frame). The decode hook and
  user pointer detach, film audio pointers clear, and the game driver is ready.
  The movie texture remains READY and identical for another 120 fields;
  retained-image SHA256 is
  `1e3be6103bd9c784a46c0159c225954aeec1b5f9de239e4e17afaba5000fe00f`.
- Resident-audio and CD-loading host regressions passed.
- `source ~/.yaul.env && make -j8 -f Makefile.saturn`: passed, including the
  no-soft-float check; rebuilt `build/saturn/saber_rider.bin` and `.cue`.

Hardware has not been retested for this change.
