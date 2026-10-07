# Saturn SV24 presentation and audio fixes

Addresses `ISSUES_SV24.txt` and the remaining work in
`codex-session-01a11743-7d72-79d3-9928-797dd5521251.md`.

The intro keeps the RGB24 SV24 decoder and VDP2 presentation. Briefing and
power clips decode the same format but display through RGB555 VDP1 sprites,
matching gameplay. They preserve the background planes, colour-depth registers,
and level VRAM throughout playback and close. No level graphics reload is needed
when a power clip ends. Pending renderer lists finish before reserving or releasing
the movie's VDP1 texture area.

UI pixels are converted in cell order, then transferred as eight-row strips from
the master's high-RAM stack through SCU DMA level 1. This avoids another heap
buffer and preserves VDP1's DMA level 0 callback. UI audio starts after the first
movie command list finishes displaying, rather than before VDP1 draws it.

The briefing source is 192x78, displayed at the original 256x104 size and exact
aspect ratio. Both power sources are 160x112, displayed at 320x224. These native
sizes leave CPU and RAM headroom for the room, dialogue, and resident cut-in art.
UI clips therefore use gameplay's 15-bit colour precision; the intro remains
24-bit. Intro packet encoding averages approximately 219 KB/s rather than
276 KB/s, leaving bandwidth for stock-CD command overhead.

Saber and Fireball retain their selected portrait and just their own baked
background cut-in frame. Level textures become resident before movie buffers are
allocated, preventing a late reload into fragmented heap gaps. Saturn explicitly
plays each hero's movie only on the first power use of a stage; later uses draw
the resident artwork without reopening the disc.

Audio uses the programmed SCSP OCT/FNS ratio (23987.98828125 Hz for requested
24 kHz), four queued intro packets, and an independently advancing audio producer.
A full ring no longer postpones video decoding. Deferred tail packets still feed
while the last picture is held. A stall exceeding the ring capacity re-primes from
the next picture with fresh DSP history and no ADX-header requirement. Segment
clock rounding is clamped to avoid a repeated recovery at the restart boundary.

## Validation

- Saturn stock-console build and disc generation pass, including the no-soft-float
  link check. Output: `build/saturn/saber_rider.cue`.
- `run_movie_lifecycle.py`: cleanup, retained surfaces, overwritten queue slots,
  sample timing, starvation recovery, restart rounding, deferred UI audio startup,
  and VDP1 close without VDP2 restoration.
- `run_power_cutin.py`: every hero and bomb variant, matching resident portrait and
  background frame, first-use movie and subsequent drawn cut-in.
- `run_svm_codec.py build/saturn/stage/*.SVM`: all 1,316 asset packets and malformed
  packet checks under UBSan.
- `run_resident_audio.py`: all 32 banks and movie sound-CPU handoff/bank retention.
- Shared `gfx.c`, `game.c`, and `power.c` pass non-Saturn syntax checks.
- Ymir CD Block LLE: full 905-frame intro completes on NTSC and PAL50 without
  drops, decoder errors, or audio underruns. Both full power clips and subsequent
  drawn cut-ins complete without decoder errors, underruns, or locked asset misses.
- PAL briefing renders the opening picture and room correctly, releases audio,
  and retains identical displayed movie pixels over a 300-field comparison.
  Three late picture frames were skipped during its opening UI animation; there
  were no decoder errors or audio underruns.

Captures and metrics are under `artifacts/saturn-sv24/`: `verified-firstframe-*`
for the final player, `verified-intro-final-pal` for the full PAL intro, and
`review-saber` / `review-fireball` for close captures of both second-use portraits.
The Mednafen hold script was updated for the VDP1 surface, but its local runner
exited during disc load; the retained-frame check above used Ymir instead.
Real-hardware playback remains to be checked.
