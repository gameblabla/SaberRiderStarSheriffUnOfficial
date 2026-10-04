#!/usr/bin/env python3
"""The floor's projection parameters: Q3 start position and per-sample step of each of the 24 BAT rows for every one of the 128 headings
(integer arithmetic on the target, no floating point). The binary goes into the race archive (build_assets.py)."""
import math
import struct
HEADINGS = 128
DISTANCE = [593,480,403,348,305,272,246,224,206,190,177,165,155,146,138,131,124,119,113,108,104,100,96,92]
STRIDE = [45,37,31,27,23,21,19,17,16,14,13,13,12,11,11,10,9,9,9,8,8,8,7,7]
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
