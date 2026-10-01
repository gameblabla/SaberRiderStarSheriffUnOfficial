# libyaul Cinepak dependency

Vendored from `razor85/libyaul_cinepak` (source archive commit `ed20258`) and adapted for the Saber Rider Saturn port.

The Saber Rider integration keeps Sega FILM/CPK Cinepak frames intact (including the Sega 16-bit frame-header padding), decodes ADX audio through the bundled Saturn sound driver, uses the game-owned FRT clock, parses unaligned 24-bit Cinepak strip headers bytewise, avoids asynchronous codebook-copy hazards, and can read movie bytes through the game's existing CDFS/stdio streamer so the decoder does not compete for CD-block ownership.

The movie audio no longer goes through pcmsys' 68000 driver: film_snd.c keeps pcmsys.h's SCSP register helpers but
plays its ring on SCSP slots the game programs directly (film_pcm_* in src/platform/saturn/aud_sat.c, the 68000
running an idle loop for the clip per Sega Technical Bulletin #51, the game's adp68k driver started again after it),
so `cd/SNDDRV.BIN` is not on the disc. Movie timer C is polled with 16-bit register accesses and no main-CPU interrupt.
