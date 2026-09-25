# libyaul Cinepak dependency

Vendored from `razor85/libyaul_cinepak` (source archive commit `ed20258`) and adapted for the Saber Rider Saturn port.

The Saber Rider integration keeps Sega FILM/CPK Cinepak frames intact (including the Sega 16-bit frame-header padding), decodes ADX audio through the bundled Saturn sound driver, uses the game-owned FRT clock, parses unaligned 24-bit Cinepak strip headers bytewise, avoids asynchronous codebook-copy hazards, and can read movie bytes through the game's existing CDFS/stdio streamer so the decoder does not compete for CD-block ownership.

`cd/SNDDRV.BIN` is part of the vendored dependency and is copied into the disc image by `tools/saturn/build_disc.py`.
