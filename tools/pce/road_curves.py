"""Bake the fixed circuit's classic road; camera corrections remain native.

Each 128-byte record has 48 signed pixel centres for the raster's even indices
16..110, and the reference camera x, y, heading. Sixteen records per segment
allow a continuous four-bit interpolation at runtime. No sprite data changes.
"""
import re, struct
from pathlib import Path

SAMPLES=4096
STRIDE=128

def bake(track_blob):
    points=list(struct.iter_unpack('<HHB',track_blob))
    source=(Path(__file__).resolve().parents[2]/'src/platform/pce/race_math.h').read_text()
    sin=list(map(int,re.findall(r'-?\d+',source.split('SIN[256]')[1].split('={',1)[1].split('};',1)[0])))
    assert len(points)==len(sin)==256
    lines=[min(255,round(10080/max(2*k,1))) for k in range(384)]
    def wrap(a,b):return ((a-b+4096)&8191)-4096
    def sine14(h):
        i=(h>>8)&255;return sin[i]*128+((sin[(i+1)&255]-sin[i])*(h&255)>>1)
    def byte(v):return ((v+128)&255)-128
    def dots(side,d):
        sm=abs(side);A=(sm>>8)*d;B=(sm&255)*d
        y=((A*77)>>1)+(B>>3)+(B>>6)+(B>>7)+(B>>9)
        y=min(y,7680 if d<64 else 12800)
        return -y if side<0 else y
    def line(f):return lines[min(f>>1,383)]
    records=[]
    for sample in range(SAMPLES):
        ps=sample<<4;i=ps>>8;fr=ps&255;n=(i+1)&255
        px=(points[i][0]+(wrap(points[n][0],points[i][0])*fr>>8))&8191
        py=(points[i][1]+(wrap(points[n][1],points[i][1])*fr>>8))&8191
        q=(ps-128)&65535;j=q>>8
        heading=((points[j][2]<<8)+byte(points[(j+1)&255][2]-points[j][2])*(q&255))&65535
        c,s=sine14(heading+16384),sine14(heading)
        cc,ss=(c+64)>>7,(s+64)>>7
        cx=px-(cc*92>>7);cy=py-(ss*92>>7)
        bi=min(((i+d)&255 for d in range(-2,4)),key=lambda j:abs(wrap(px,points[j][0]))+abs(wrap(py,points[j][1])))
        index=(bi-3)&255;forward=sideq=0;previous_f=-32000;previous_side=0;knots=[]
        for k in range(13):
            if not k:
                rx,ry=wrap(points[index][0],cx),wrap(points[index][1],cy)
                forward=rx*c+ry*s;sideq=ry*c-rx*s
            else:
                step=1 if k<=7 else 2;next_=(index+step)&255
                dx=byte(points[next_][0]-points[index][0]);dy=byte(points[next_][1]-points[index][1]);index=next_
                forward+=dx*c+dy*s;sideq+=dy*c-dx*s
            f=forward>>14;side=max(-1200,min(1200,sideq>>12))
            if f<=previous_f:break
            if f<88:previous_f,previous_side=f,side;continue
            d=line(f);x=dots(side,d)
            if not knots and previous_f>-32000:
                t=min(127,((88-previous_f)<<7)//(f-previous_f))
                s0=previous_side+((side-previous_side)*t>>7)
                knots.append((line(88),dots(s0,line(88))))
            knots.append((d,x));previous_f,previous_side=f,side
            if f>=560:break
        centres=[0]*256;dcur=110
        def fill(end,x,slope):
            nonlocal dcur
            for d in range(dcur,end,-1):
                centres[d+1]=x+slope*(dcur-d)
            dcur=end
        if knots:
            dp,xp=knots[0]
            if dcur>dp:fill(dp,xp,0)
            for dq,xq in knots[1:]:
                n=dp-dq
                if n<=0:continue
                delta=xq-xp;slope=(abs(delta)//n)*(-1 if delta<0 else 1)
                fill(dq,xp+slope*(dp-dcur),slope);dp,xp=dq,xq
                if dcur<1:break
            fill(-2,xp,0)
        values=[(centres[m]+15)//16 for m in range(16,112,2)]
        records.append(struct.pack('<48h3H',*values,cx&8191,cy&8191,heading)+bytes(26))
    max_delta=max(abs(struct.unpack_from('<h',records[(i+1)&4095],m)[0]-struct.unpack_from('<h',r,m)[0]) for i,r in enumerate(records) for m in range(0,96,2))
    assert max_delta<=127, ('native byte interpolation overflow',max_delta)
    print(f'  baked road: {SAMPLES} curves, {SAMPLES*STRIDE} bytes; maximum neighbour pixel delta {max_delta}',flush=True)
    return b''.join(records)
