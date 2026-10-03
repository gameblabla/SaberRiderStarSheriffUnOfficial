# PCE sound reference and current implementation

The game ships timer-driven **5-bit PCM at approximately 6.99 kHz**, using
original shooting/impact/power samples. It preserves the hardware ADPCM voice
for jump/hurt/death. `src/platform/pce/audio_pcm.S` is a resident IRQ handler;
its sample banks `$7d–$7f` are outside the loader's `$75–$7c` scratch banks.
The BIOS timer hook jumps to its handler, so it returns with RTI. VDC callbacks
use RTS. The PCM IRQ preserves A and MPR6 and uses no compiler scratch registers.

The timer stops at sample end and during every blocking scene load. Voice-only
events never start a PSG tone. Death waits for hardware voice completion before
reload. `build/pce/audio-review/` contains actual emulator output WAVs for the
shot, impact, stop-during-shot, and jump/hurt/death for all four heroes. Native
isolated checks delivered 116 samples per video frame and ended with a silent
tail. The other heroes are loaded through real menu inputs before isolation.

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

The reported 17% CPU use is not measured here. The game's race renderer already
owns scanline scheduling; the demo's scanline delivery driver needs a combined
scheduler before integration. The game therefore uses the requested PCM fallback.
