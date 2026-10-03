"""Greedy MSM5205 encoder: 12-bit wrapping predictor, high nibble first.

Hardware semantics checked against Mednafen sound/okiadpcm.h and PCECD_Run.
The step sizes and index adjustments are the chip's fixed coding parameters.
"""
STEP=[int(16*1.1**i) for i in range(49)]
ADJUST=[-1,-1,-1,-1,2,4,6,8]*2

def delta(step,n):
    d=(step>>3)+((step>>2) if n&1 else 0)+((step>>1) if n&2 else 0)+(step if n&4 else 0)
    return -d if n&8 else d

def decode(data):
    predictor=index=0;out=[]
    for byte in data:
        for n in (byte>>4,byte&15):
            predictor=((predictor+delta(STEP[index],n)+2048)&4095)-2048
            index=max(0,min(48,index+ADJUST[n]));out.append(predictor*16)
    return out

def encode(pcm):
    predictor=index=0;nibbles=[]
    for sample in pcm:
        target=int(sample)//16
        candidates=[((predictor+delta(STEP[index],n)+2048)&4095)-2048 for n in range(16)]
        n=min(range(16),key=lambda n:abs(candidates[n]-target))
        predictor=candidates[n];index=max(0,min(48,index+ADJUST[n]));nibbles.append(n)
    if len(nibbles)&1:nibbles.append(8)
    return bytes((a<<4)|b for a,b in zip(nibbles[::2],nibbles[1::2]))
