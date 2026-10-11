# Saturn movie audio and quality follow-up

Source changes only, at the user's request. No builds, asset baking, tests,
emulator runs, or hardware checks were performed for this follow-up. Earlier
session validation does not establish that these changes work on hardware.

The previous path fed 24 kHz ADX residuals into a recursive predictor executing
at the SCSP DSP's 44.1 kHz rate. It advanced history on interpolated feeder
samples rather than once per compressed source sample. Movies now reconstruct
ADX on the SH-2 and play ordinary stereo PCM through directly owned SCSP slots.
Predictor history advances exactly once per source sample. Newly baked SVMs
store the encoder's truncated Q13 coefficients, identified by 13 at header
offset 54. Older half-Q12 coefficients are normalized by the SVM reader.

The PCM rings hold 32,768 samples per channel (about 1.36 seconds at 24 kHz).
The upper 128 KB of sound RAM is reserved for them, from 0x60000 through
0x7FFFF. Runtime and baker resident-bank limits agree. The existing audio
manifest's largest resident bank is 315,120 bytes, below the new 384,000-byte
bank limit. The game feeder adapter uses the same ring definitions.

The intro and full-height movies prefetch twelve packets instead of four;
the briefing prefetches six instead of three. Audio feeding runs even on fields
where no picture needs decoding. Exceptional starvation stops stale PCM,
retains ADX history, resumes at the first undecoded audio packet, and rebuilds
at least one third of a second of lead before restarting (or the remaining
tail at EOF). Audio packets are not replayed from the next picture. Queue
reuse checks packet count as well as packet size. Initial audio key-on waits
until the first renderer command list has finished, for both presentation paths.

Power assets now bake at 320x224 and display through RGB24 VDP2 NBG0. Their
encoding budget rises from 8,000 to 16,000 video bytes per frame. The existing
VDP2 ownership/level restoration path is reused; full-screen movies temporarily
occupy level VRAM, and restoration may still introduce a transition delay.
Their staging omits unused padding columns but stays in high work RAM for DMA.

The briefing bakes at its displayed 256x104 resolution rather than 192x78.
It retains RGB555 VDP1 presentation to preserve the room's background planes.
Every movie suspends CDDA, including RAM clips. Saturn's menu requests briefing
music once, after playback finishes or after the clip is closed by a skip.
Other platforms keep their existing music timing.

A new disc build is required for the changed asset sizes, coefficients, sound
bank manifest, and executable. Existing disc images have not been updated.

Power-up music follow-up: movie startup previously called the permanent CDDA
stop, clearing the track that gameplay expected to keep playing. The CD owner
now has a separate movie suspension gate. It retains track, FAD, loop and user
pause intent, and prevents CDDA start/update from reclaiming the drive during
playback. New music requests during suspension replace the retained intent;
explicit stop still cancels it. Movie cleanup restores the ADP68K driver and
publishes CD input gain before releasing the gate. The normal audio update
resumes music once data reads finish. Completion, skip and activation failure
share that cleanup. Briefing music remains deferred by the menu's existing
explicit stop and post-movie request. The unused movie CD passthrough mode was
removed. This follow-up also involved no builds or tests.
