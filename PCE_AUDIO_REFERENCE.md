# PCE sound reference and current implementation

The game decodes the supplied Build 14 **2-bit software ADPCM** through the
HuC6280 timer IRQ at approximately **6.99 kHz**, writing the predictor's high
five bits to independent PSG DDA channels:

| PSG channel | Effects |
| --- | --- |
| 0 | Original gunfire / impact / power / UI effects (latest request replaces the previous effect) |
| 1 | Original horse gallop (`SFX_TABLE[28]`, `82EFBA26`), looping while the herd lives |

Gunfire and impacts do not interrupt the gallop. Each stream has its own
16-bit saturated predictor, step index, packed-byte phase and exact sample
count. A loop resets its pointer, predictor and index, including when its
last byte contains fewer than four samples. One-shot effects disable only
their own channel; the timer stops when both channels are idle.
`audio_pcm_stop()` stops all software voices before blocking loads.

`src/platform/pce/audio_pcm.S` is the resident timer entry plus a banked native
decoder. The BIOS timer hook jumps to it, so it returns with RTI. It preserves
A/X/Y and MPR3/MPR6. It uses private direct-page state at `$2080–$20a0`,
touches no compiler registers, and does not change VDC registers or raster scheduling. Code and exact ROM tables use
bank `$75`; compressed samples use `$7d–$7f`. The loader uses `$76–$7c` as a
56 KiB CD scratch buffer, preserving the decoder and samples across loads.
The existing CD-DA music and CD hardware ADPCM character voices remain separate
from these software-decoded PSG channels.

The demo's weighted two-channel 8-bit output and scanline delivery are not
used by this game driver. One 5-bit DAC per voice lets the gallop overlap
one one-shot effect without taking over the race raster.

Profiling twelve isolated video frames measured these percentages of all CPU
cycles. Decoder percentages exclude the resident IRQ wrapper and BIOS dispatch;
handler percentages include the wrapper but still exclude BIOS dispatch.

| Driver | One voice: decoder | Two voices: decoder | Two voices: handler |
| --- | --- | --- | --- |
| Two channels before optimization | 26.31% | 44.18% | — |
| Direct-page state / packed-byte optimization | 18.18% | 32.99% | 40.90% |
| Final optimized driver | 17.21% | 31.24% | 37.59% |

The final one-voice handler measured 23.56%. The previous three-channel driver
measured 65.70% decoder time with all three voices playing. These measurements
are isolated sample-delivery costs, not gameplay rendering throughput or the
demo's claimed 17% total budget. Reproduce with
`python3 tools/pce/profile_audio.py --out build/pce` (`audio-profile.json`).
Historical measurements are saved under `build/pce/audio-optimization/`.

The optimized driver reserves dedicated direct-page state, protected by the
linker's compiler-ZP boundary assertion. It uses an active-channel mask,
HuC6280 bit branches, indexed-indirect sample reads, and a four-sample countdown.
Packed-byte fetches locally save/restore MPR6 once per four samples. Sign/size
branches reuse the code bits' carry flag for exact ADC/SBC; sample-count updates
check the high byte only when needed. The rate, codec tables, saturation and
5-bit output are unchanged.

`build/pce/audio-review/` contains actual emulator WAV captures for isolated
effects, concurrent voices, the looping gallop, and stop checks.
`test_audio.py` checks native sample delivery, mapping restoration, concurrent
channels, looping, silence after stopping, and all four heroes' voices.
`test_adpcm2.py` compares the host codec with the supplied ROM's decoder;
`test_software_adpcm.py` checks 4,430 native game decoder samples/states across
both channels, saturation limits, code combinations and partial bytes.

The herd spawns before the camera locks. The exported PC camera-stop zones
are x=2392, 6292 and 8976; spawn triggers are x=2252, 5916 and 8648.
Starting distance follows each trigger's interval (195, 435 and 420 pixels).
`test_herd.py` seeds each approach, then executes native scrolling, spawning,
locking, gallop playback and release with no mid-sequence disc reads. The
PCE adaptation still uses five horses spaced 192 pixels apart to respect the
VDC sprite budget.

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
