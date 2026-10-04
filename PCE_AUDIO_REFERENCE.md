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

`src/platform/pce/audio_pcm.S` is the resident timer entry plus a playback
service in the always-mapped work bank `$6b`. The BIOS timer hook jumps to it,
so it returns with RTI. It preserves A and MPR6, leaves X/Y and MPR3 untouched,
and uses private direct-page state at
`$2080–$20a0`. It touches no compiler registers, VDC registers or raster scheduling.
MPR6 is saved once per interrupt across both voices. Streams cross `$dfff` into
`$c000` in the next bank; looping restores both the start pointer and start bank.
The timer stops when both channels are idle, and `audio_pcm_stop()` stops every
voice before blocking loads.

Audio controls and 7,006 shot/power DAC bytes share bank `$75`.
The two fixed voice paths are unrolled in `$6b`, avoiding per-voice calls,
indexed state access and interrupt-time overlay switching. Impact and gallop
occupy 23,041 bytes across `$7d–$7f`. The linker checks bank capacity. The loader
retains its 56 KiB scratch buffer in `$76–$7c`, preserving audio across loads.
The existing CD-DA music and CD hardware ADPCM character voices remain separate. Each hero's ADPCM bank
(`voiceN.bin`, `voice_groups` / `voice_samples` in the generated `samples.h`) holds 15 samples in 8 events: jump, hurt x3,
death x2, fall, enemy hit x3, enemy death x3, the Outrider's alarm and the level-1 dialogue line. The PC game's
random variant choice is reproduced by `voice_pick` (bank `$75`); priorities: dialogue > death/fall > hurt > the rest.

Twelve isolated video frames measured these percentages of all CPU cycles.
Service percentages exclude the resident IRQ wrapper and BIOS dispatch;
handler percentages include the wrapper but still exclude BIOS dispatch.

| Driver | One voice: service | Two voices: service | Two voices: handler |
| --- | --- | --- | --- |
| Previous optimized runtime decoder | 17.21% | 31.24% | 37.59% |
| Exact DAC bytes decoded during build | 10.26% | 17.30% | 24.34% |
| Unrolled delivery in the work bank | 8.70% | 14.37% | 18.47% |

The current one-voice handler measured 12.80%. The two-voice handler uses
24.1% fewer cycles than the first predecoded driver (24.34%), retaining the
same DAC bytes, sample rate and loop behavior. These exclude BIOS dispatch.
Reproduce with `python3 tools/pce/profile_audio.py --out build/pce`.

The herd renderer now sustains 60 Hz with held fire in all three seeded
encounters: 300 completed draws and 300 new presentations at VBlank over
300 video frames, with no essential sprite overflows. Playback accounts for
about 18.47% of cycles in these encounters. The previous render-loop measurement
was 113/300 draws (22.6 fps). See `PCE_HERD_60FPS.md` for the scenario, visual
and sound equivalence checks, and reproduction commands. Other gameplay
scenarios are covered by functional tests, rather than this performance claim.

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
