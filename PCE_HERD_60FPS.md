# PCE herd: one new presentation per video refresh

The three level-1 stampedes pass the strict 60 Hz check with continuous
firing: **300 completed draws and 300 new presentations in 300 video frames**
for each encounter, with **zero essential sprite overflows**. The prior
session's first-herd render-loop measurement was **113/300 (22.6 fps)** at
commit `a608839`. Rates use the same normalization to 60 video frames/s.

| Herd trigger X | Completed draws | New presentations at VBlank | Repeated presentations | Essential overflows | Sprite-cache uploads during firing |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2252 | 300 | 300 | 0 | 0 | 0 bytes |
| 5916 | 300 | 300 | 0 | 0 | 0 bytes |
| 8648 | 300 | 300 | 0 | 0 | 0 bytes |

## Scenario and measurement

`tools/pce/profile_gameplay.py` uses the supplied accurate-core Mednafen
emulator with Arcade Card enabled and the real VDC sprite limit enabled.
It seeds the hero 40 pixels before each herd trigger, clears the actor pool,
and skips dialogs. Native game code spawns the horses, advances their
animation, scrolls the camera, and enters the camera-stop zone. Measurement
starts at a completed draw of the locked scene, then immediately holds fire
for 300 video frames. Safety is refreshed to keep the full measurement
inside the encounter. The default sheriff is Saber.

`pce_draws` counts completed SAT submissions. `pce_presented` observes their
generations in the VBlank callback, so repeated display frames cannot be
hidden by simulation catch-up or by multiple draws between refreshes.
The strict assertion requires exactly one new presentation in every measured
video frame, plus zero essential sprite overflows. It includes the start of
firing and uses no firing warm-up period.

This establishes the frame budget for these seeded herd encounters. It is
not a measurement of every enemy arrangement, power attack or other stage,
and hardware-console timing has not been separately measured.

## Implementation

- `arcade_fast.S`: map Arcade data-port mirror bank `$40` into MPR6 and use
  TIA to alternate the VDC low/high data ports. A native loop replaces the C
  loop and byte-at-a-time transfers. Bursts are at most 64 bytes, leaving
  time for the PCM timer between transfers. MPR6 is restored on return.
- `herd_prepare.S`, `herd_emit.S`, `sprite_lines.S`: collect clipped horse
  columns once and emit SAT records directly. Horses sharing a Y span reserve
  their total scanline capacity together. Exact scanline accounting and the
  original top/bottom/column/horse order are preserved. Different heights,
  overlapping 8-line bands and tight SAT space use the original per-horse or
  per-cell admission path. Sixteen-line pieces use unrolled admission and
  cached remaining capacity; every other occupancy mutation invalidates it.
- `herd_pce.c`: prepare the next **complete** 5120-byte horse frame in three
  slices across the existing two buffers. The displayed buffer is reused
  only after its final VBlank. The five original animation frames still switch
  every four simulation ticks. During the approach, retain both firing poses,
  the four straight muzzle frames and the projectile so firing needs no
  pattern reloads. Release their extra pins when the herd ends.
- `presentation_pce.c`, `hud_copy.S`: retain the unchanged HUD's SAT entries
  and scanline contribution. Counters, sheriff, HP, occupancy mode and sprite
  cache identities guard reuse; changed HUDs rebuild normally. Copies use
  short chunks, allowing timer service between instructions. Foreground-entry frames check the occupancy
  prefix before replay.
- `sat_copy.S`: upload SAT segments with atomic VDC setup and transfers of
  at most 64 bytes, so the timer can run between chunks instead of waiting
  through an entire sprite table. The original SAT segment order is retained.
- `sprite_cache_pce.c`, `sprite_cache_begin.S`: prefer empty cache slots and
  pages, then older unpinned owners. Preserve both displayed generations and
  reserved herd pages. Cache the scene's foreground threshold and use the
  existing ID-to-slot map for lookup.
- `audio_pcm.S`: place unrolled voice delivery in always-mapped bank `$6b`.
  The IRQ preserves A/MPR6 and leaves X/Y/MPR3 untouched. It sends the same
  predecoded DAC bytes at the same timer rate, with the same bank crossings,
  end counts and loops. CD-DA music and hardware ADPCM voices remain active.

The Arcade Card bank-mirror/TIA route is documented by the
[HuC maintainers](https://github.com/pce-devel/huc/wiki/Arcade-Card#arcade-card-bank-data-registers).

## Equivalence and regression checks

`test_herd_render.py` executes the native renderer against the original
per-cell admission model for **160 cases**: both occupancy modes, both SAT
pages, horizontal/vertical clipping, mixed horse heights, saturated scanlines,
and SAT counts up to 64. SAT bytes and every occupancy byte match. It compares
all five streamed horse frames with the original scene bytes, checks animation
wrap/skipped ticks, and verifies the previously displayed buffer stays intact
on animation switches. **32 HUD cases** compare fresh native builds with
retained replay across all four sheriffs, all HP states and both occupancy modes.

`test_software_adpcm.py` compares **30,047 generated DAC bytes** and **1,590
native playback samples**, including counter borrow, bank crossing and loop
reset. `test_audio.py` checks actual emulator WAV captures, sample rate,
concurrent gunfire/gallop, hardware voices, mapping restoration and stop silence.
The current isolated two-voice handler measures about **18.47%** of cycles,
versus **24.34%** before this change, excluding BIOS dispatch.

The full PCE suite covers all seven stages, campaign transitions, three herd
triggers/locks/releases, foreground priority, HUD/aim poses, audio and codec
checks. The ELF check also verifies every bank and console-RAM boundary.

## Reproduce

```sh
make -f Makefile.pce test-herd
make -f Makefile.pce test
python3 tools/pce/profile_audio.py --out build/pce
```

Per-herd reports are `build/pce/gameplay-profile-herd{1,2,3}.json`;
`herd-render-verification.json` records native equivalence. The rebuilt disc is
`build/pce/saber_rider.cue` / `build/pce/saber_rider.iso`.
