This directory vendors the source portions of celeriyacon/scspadpcm supplied for the Saturn port.
Saber Rider uses its predictor/quantizer and 4-bit block format for SFX. The original proof-of-concept SCSP driver uses
four SCSP slots per voice and 16-bit SCSP playback lengths, so it conflicts with the simultaneous movie/music streams
and caps long samples. The Saturn port therefore decodes the same block codec on the SH-2 into a shared PCM output ring.
Its SADP wrapper interleaves each block and uses 32-bit sample/block counts, so long voice/effect samples are continuous
and are not capped at the original driver's 16-bit address/length range. Original license notices are retained in source.
