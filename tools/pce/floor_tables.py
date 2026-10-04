#!/usr/bin/env python3
"""The floor's projection parameters: Q3 start position and per-sample step of each of the 19 BAT rows for every one of the 128 headings
(integer arithmetic on the target, no floating point). The binary goes into the race archive (build_assets.py)."""
import math
import struct
HEADINGS = 128
# The floor is drawn from scanline 120 (the horizon is at 113). The seven far strips are 8 scanlines tall (sampled at half
# resolution), the twelve near ones 4: distance = 10080 / (scanlines below the horizon at the strip's middle).
ROWS = 19
FAR_ROWS = 7
FIRST_LINE = 120
DISTANCE = [916,530,373,288,234,198,171,155,146,138,131,124,119,113,108,104,100,96,92]
STRIDE = [70,40,28,22,18,15,13,12,11,11,10,9,9,9,8,8,8,7,7]
def trunc(a,b):
    return (abs(a)//b)*(-1 if a<0 else 1)
def generate():
    out=bytearray()
    for h in range(HEADINGS):
        sn=round(127*math.sin(2*math.pi*h/HEADINGS));cs=round(127*math.cos(2*math.pi*h/HEADINGS))
        for distance,stride in zip(DISTANCE,STRIDE):
            du=trunc(-sn*stride,128);dv=trunc(cs*stride,128)
            out+=struct.pack('<4h',trunc(cs*distance,16)-du*64,trunc(sn*distance,16)-dv*64,du,dv)
    return bytes(out)
