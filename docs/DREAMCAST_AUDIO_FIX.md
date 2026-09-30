# Dreamcast victory music and turbo audio

Source review and fix, 2026-09-30. No build, tests, emulator, audio decoding
experiment, or hardware run was performed, as requested. These are fixes for
faults identified in the source; their effect on the reported hardware symptoms
has not been measured.

## References reviewed

- Local hardware documentation: `dreamcast-docs/hardware/aica.md`,
  `dreamcast-docs/files/official/AICA_E.pdf` (Loop Control and Method of Decoding),
  `dreamcast-docs/files/aica/aica_v08.txt` (sample formats and loop addresses),
  and `dreamcast-docs/files/official/SH4_access990312_e.pdf` (G2/AICA accesses).
- `dreamcast-docs/software/kos.md` and the installed KOS sources under
  `/opt/toolchains/dc/kos/kernel/arch/dreamcast/`: `sound/snd_stream.c`,
  `sound/snd_sfxmgr.c`, `sound/snd_iface.c`, `sound/arm/aica.c`,
  `include/dc/sound/stream.h`, and `fs/fs_iso9660.c`.
- Installed KOS `utils/wav2adpcm/wav2adpcm.c`, including its public domain
  low-nibble-first YMZ decoder and initial predictor/step state.
- The installed port selects libADX v1.0.1. Its matching upstream source was
  read at commit `27574de545cb59104e42a53282c73fe208b73bd0`:
  [decoder](https://github.com/sega-dreamcast/libadx/blob/v1.0.1/src/libadx.c),
  [driver](https://github.com/sega-dreamcast/libadx/blob/v1.0.1/src/snddrv.c).
- ADX header cutoff coefficients and block interpretation were also checked
  against FFmpeg's [coefficient calculation](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/adx.c)
  and [decoder](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/adxdec.c).

## Faults found

KOS prefills a stream with two half-buffer callbacks. The old effect callback
returned a short sample tail without filling the rest of that request. KOS
transfers only the returned bytes; it does not clear the remainder after a
nonempty callback. Turbo's approximately 11 KiB encoded loop therefore left
part of the 16 KiB AICA buffer unwritten on startup. Aligning the sample's total
size to 32 bytes did not fix that.

`snd_stream_start_adpcm` uses the continuous ADPCM format. Repeating an entire
sample encoded with an initial history of zero and step size of 127 inside that
stream does not restore that state at the sample boundary. The official manual
describes how each decoded sample depends on the preceding history and step.
This is an additional cause of changing noise at turbo loop boundaries.

libADX's callback returned its requested length even when `BUFEND` released it
without a fresh complete PCM buffer. Its decoder discarded a final tail below
16 KiB, and its EOF drain loop could spin with exactly the requested amount
remaining because it used `>` instead of `>=`. The driver set its status to NULL
before destroying its stream and calling global `snd_stream_shutdown`. Our link
wrappers suppressed the global shutdown and muted after DONE, but did not give
us ownership of the final PCM tail or a joinable driver lifecycle.

The old decoder also used small stdio reads for compressed ADX groups. KOS's
ISO9660 implementation uses a bulk sector read when the destination is aligned
to 32 bytes, the file is at a sector boundary, and the request includes full
sectors. The new compressed read-ahead uses that path.

## Changes

`aud_dc.c` now supplies every effect request in full, filling ADPCM one-shot
tails with `0x80`. Loops are decoded once using the baker's KOS YMZ algorithm
and repeated as PCM16, filling across every wrap. Each effect has two aligned
scratch buffers because KOS can still be DMAing the previous callback's source.
The effect ring uses KOS's 32,704-byte ADPCM limit, also providing about 0.742
seconds of PCM turbo at 22.05 kHz with a refill margin of about 0.371 seconds.
Mono starts and their initial volume are queued together, including turbo's
initial gain of zero.

`music_adx.c` handles the unencrypted version-3, encoding-3, 18-byte, 4-bit mono/stereo ADX
layout emitted by our baker. It validates the header, honors its sample count,
derives predictor coefficients from its cutoff and rate, retains a partial
decoded group, and resets predictor history when repeating the complete track.
Embedded ADX loop points and encrypted or other ADX versions/encodings are outside the
baker's format contract.

Tracks at most 512 KiB are read into aligned main RAM before audio starts, then
their file is closed. The existing baked files are `AF162C58.adx` (mission
jingle, 457,542 bytes) and `C93B9F7F.adx` (victory screen, 448,398 bytes), so
neither performs disc reads during playback. Their headers were inspected as
encoding 3, 18 bytes, 4 bits, stereo, 44.1 kHz, 500 Hz cutoff, version 3.
Larger tracks have a joinable reader and two aligned 64 KiB compressed buffers.
The reader finishes or is joined before its file and buffers are released.

The music worker exclusively starts, polls, adjusts, and stops one persistent
KOS PCM stream. Its 64 KiB per-channel AICA ring holds about 0.743 seconds at
44.1 kHz, compared with libADX's 16 KiB ring (about 0.186 seconds). At EOF the
last PCM samples are followed by zeros, polling continues for a full ring's
duration plus 50 ms, then the stream stops. Stop/start wishes and gain are
protected by the same mutex, and an open superseded by a newer request is
discarded before starting playback. KOS stream initialization and shutdown
happen only at the backend's lifetime boundaries. The libADX link dependency
and wrappers are removed.

The existing `.adx` files and `snd.pck` format are retained; the next ELF build
can be repacked into the existing disc without an audio asset rebake.
