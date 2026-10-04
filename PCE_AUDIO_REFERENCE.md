# PCE sound reference and current implementation

The asset builder encodes the supplied Build 14 **2-bit software ADPCM**, then
expands its exact output to **5-bit DAC bytes**. The HuC6280 timer IRQ delivers
those bytes at approximately **6.99 kHz** to independent PSG DDA channels:

| PSG channel | Effects |
| --- | --- |
| 0 | Gunfire / impact / power / UI effects (latest request replaces the previous effect) |
| 1 | Horse gallop (`SFX_TABLE[28]`, `82EFBA26`), looping while the herd lives |

The DAC values, sample count, sample rate and loop boundary are identical to the
previous native decoder. Build-time expansion removes predictor arithmetic,
adaptation-table reads and packed-code extraction from the interrupt. No disc
reads or runtime decompression are needed when starting an effect.

`src/platform/pce/audio_pcm.S` is the resident timer entry plus a banked playback
service. The BIOS timer hook jumps to it, so it returns with RTI. It preserves
A/X and MPR3/MPR6, leaves Y untouched, and uses private direct-page state at
`$2080–$20a0`. It touches no compiler registers, VDC registers or raster scheduling.
MPR6 is saved once per interrupt across both voices. Streams cross `$dfff` into
`$c000` in the next bank; looping restores both the start pointer and start bank.
The timer stops when both channels are idle, and `audio_pcm_stop()` stops every
voice before blocking loads.

Audio code and 7,006 shot/power DAC bytes share bank `$75`. Impact and gallop
occupy 23,041 bytes across `$7d–$7f`. The linker checks bank capacity. The loader
retains its 56 KiB scratch buffer in `$76–$7c`, preserving audio across loads.
The existing CD-DA music and CD hardware ADPCM character voices remain separate.

Twelve isolated video frames measured these percentages of all CPU cycles.
Service percentages exclude the resident IRQ wrapper and BIOS dispatch;
handler percentages include the wrapper but still exclude BIOS dispatch.

| Driver | One voice: service | Two voices: service | Two voices: handler |
| --- | --- | --- | --- |
| Previous optimized runtime decoder | 17.21% | 31.24% | 37.59% |
| Exact DAC bytes decoded during build | 10.26% | 17.30% | 24.34% |

The new one-voice handler measured 17.29%, down from 23.56%. The two-voice
handler uses **35.2% fewer cycles**. These are isolated delivery costs, not
whole-game frame-rate guarantees. Reproduce with
`python3 tools/pce/profile_audio.py --out build/pce` (`audio-profile.json`).

`profile_gameplay.py` measures a seeded first-herd encounter with held fire for
300 video frames. Counting render-loop iterations (rather than simulation
ticks, which catch up after missed frames), the previous driver completed 75
draws; the new driver completed 113: approximately **15.0 to 22.6 fps** when
normalized to 60 video frames per second. Audio handler time fell from 37.35%
to 24.23%. This seeded encounter still drops frames; it is not a worst-case
measurement for every scene.
Run `python3 tools/pce/profile_gameplay.py --out build/pce` to reproduce.

`build/pce/audio-review/` contains actual emulator WAV captures for isolated
effects, concurrent voices, the looping gallop, and stop checks.
`test_audio.py` checks sample delivery rate, mapping restoration, concurrent
channels, looping, silence after stopping, and all four heroes' voices.
`test_adpcm2.py` compares the host codec with the supplied ROM's decoder.
`test_software_adpcm.py` checks all 30,047 DAC asset bytes against that codec,
verifies loaded banks after boot, and executes 1,590 native playback samples
covering both channels, count borrows, bank crossings, and loop bank reset.

The herd spawns before the camera locks. The exported PC camera-stop zones
are x=2392, 6292 and 8976; spawn triggers are x=2252, 5916 and 8648.
Starting distance follows each trigger's interval (195, 435 and 420 pixels).
`test_herd.py` executes native scrolling, spawning, locking, gallop playback and
release with no mid-sequence disc reads. Five horses spaced 192 pixels apart
respect the VDC sprite budget.

## Build 14 ROM disassembly

Input: `../PCE/Sound/adpcm_build_14_2bit.pce`, SHA-256
`fb190a95676fb751d8b71737bdb859c5ef3fb9e8a892aed49affe3691f2ac416`.
The analysis used the supplied accurate core's HuC6280 disassembler
`../PCE/mednafenPceDev-main/mednafen/src/pce/dis6280.cpp`.

| ROM bank / CPU address | Behavior |
| --- | --- |
| `$00:$e000` | Reset maps ROM `$02` through MPR4 and `$03` through MPR5; `$01` later occupies MPR6. |
| `$00:$e0b9–$e10a` | Scanline IRQ consumes the `$2c00` ring buffer, writes DDA channels 0/1 through two lookup tables, schedules the next RCR and returns with RTI. |
| `$00:$e200`, `$e300` | Output split: high five bits on channel 0, low three bits times four on the quieter channel 1. |
| `$00:$e864` | Timer IRQ only acknowledges and returns. Audio delivery is **not** timer driven in this demo. |
| `$01:$c540–$c55a` | Channel 0 control `$df`, channel 1 `$cb`, both with balance `$ff`: weighted two-channel DDA. |
| `$01:$c598–$c5dd` | Playback setup predecodes blocks and installs the scanline handler. |
| `$01:$c5de–$c612` | Initializes stream pointer/count, predictor `$8000` and step index 0. |
| `$01:$c62e–$ca90` | Block decoder, four low-to-high 2-bit pairs per byte; handles short final blocks. |
| `$03:$a000–$a5ff` | Small/large 16-bit magnitudes and two step-index transition tables. |

The pair coding is 0 = +small, 1 = +large, 2 = -small, 3 = -large.
Addition/subtraction saturates to unsigned `$0000–$ffff`, matching the carry
branches rather than wrapping. Each decoded output is the predictor's high byte.
Small and large codes select their respective index transition tables.

`tools/pce/adpcm2.py` reads these exact tables and provides an encoder, decoder,
raw stream output, JSON sample-count/rate sidecar and decoded WAV. A caller must
supply that sample count to the ROM-style playback metadata; the raw stream has
no embedded header. All four pairs of the final byte are valid only when the
count is a multiple of four.

`tools/pce/test_adpcm2.py` directly executes the ROM's native decoder with seeded
registers/RAM and compares 48 cases / 954 output samples, predictor values and
step indices. Cases include both saturation directions, sign/size combinations,
encoded sine data and partial-byte endings. This validates codec compatibility.

The game's timer driver reuses this codec while leaving scanline scheduling
to the renderer. The measured cost and output adaptation are described above.
