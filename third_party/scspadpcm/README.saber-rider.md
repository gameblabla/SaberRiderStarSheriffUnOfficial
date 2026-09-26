This directory vendors the source portions of celeriyacon/scspadpcm supplied for the Saturn port (scsp.h and types.h
from the same repository).

Saber Rider's game audio runs on this driver as it is: `make -f Makefile.saturn adp68k` builds adp68k.c with a 68000
gcc into src/platform/saturn/adp68k_bin.h (bit-identical to the upstream adp68k.bin), and src/platform/saturn/aud_sat.c
drives it (8 ADPCM channels decoded by the SCSP DSP, samples in a sound RAM bank, CD-DA music through its CD input).
tools/saturn/build_disc.py encodes the samples with the unmodified adpencode.cpp (looped at their start, so one copy
plays once or looped). The driver's 16-bit loop registers cap a sample at ~2.97 s at 4 bits; longer ones are encoded
at 2 or 1 bit. Original license notices are retained in source.
