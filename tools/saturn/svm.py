"""Bake finite RGB24 FRM3/DSP3 + stereo 24 kHz ADX movies for V28.1.

A content-independent rate/distortion allocator chooses temporal skips and
1/2/4/8/16-color RGB24 records. Up to five useful A24 tiles are owned by the
SCU DSP. The average per-frame payload is bounded for a stock 2x CD drive.
The source package supplies a decoder, but no complete reusable base encoder.
"""
from __future__ import annotations
import hashlib,json,math,struct,subprocess
from pathlib import Path
import numpy as np
from numba import njit
from film import ffmpeg_exe, _adx
FPS=15
RATE=24000
CACHE_VERSION=1
VIDEO_BUDGET=16000

@njit(cache=True)
def fit(tile,k):
    pal=np.empty((k,3),np.uint8)
    mean=tile.astype(np.float64).mean(axis=0) if False else np.zeros(3,np.float64)
    for i in range(64):
        for c in range(3):mean[c]+=tile[i,c]/64.0
    centers=np.empty((k,3),np.float64)
    centers[0]=mean
    for q in range(1,k):
        best=-1.;ix=0
        for i in range(64):
            dmin=1e30
            for j in range(q):
                d=0.
                for c in range(3):d+=(tile[i,c]-centers[j,c])**2
                if d<dmin:dmin=d
            if dmin>best:best=dmin;ix=i
        centers[q]=tile[ix]
    for _ in range(4):
        sums=np.zeros((k,3),np.float64);counts=np.zeros(k,np.int32)
        for i in range(64):
            best=1e30;ix=0
            for j in range(k):
                d=0.
                for c in range(3):d+=(tile[i,c]-centers[j,c])**2
                if d<best:best=d;ix=j
            counts[ix]+=1
            for c in range(3):sums[ix,c]+=tile[i,c]
        for j in range(k):
            if counts[j]:centers[j]=sums[j]/counts[j]
    for j in range(k):
        for c in range(3):pal[j,c]=min(255,max(0,int(centers[j,c]+0.5)))
    sel=np.empty(64,np.uint8);out=np.empty((64,3),np.uint8);sse=0
    for i in range(64):
        best=1<<30;ix=0
        for j in range(k):
            d=0
            for c in range(3):d+=(int(tile[i,c])-int(pal[j,c]))**2
            if d<best:best=d;ix=j
        sse+=best;sel[i]=ix;out[i]=pal[ix]
    return pal,sel,out,sse

@njit(cache=True)
def candidates(tiles,prev):
    n=len(tiles);pals=np.zeros((n,5,16,3),np.uint8);sels=np.zeros((n,5,64),np.uint8)
    outputs=np.zeros((n,5,64,3),np.uint8);errors=np.empty((n,6),np.int64)
    for t in range(n):
        e=0
        for i in range(64):
            for c in range(3):e+=(int(tiles[t,i,c])-int(prev[t,i,c]))**2
        errors[t,0]=e
        for j in range(5):
            k=1<<j;p,s,o,e=fit(tiles[t],k)
            pals[t,j,:k]=p;sels[t,j]=s;outputs[t,j]=o;errors[t,j+1]=e
    return pals,sels,outputs,errors

def record(pal,sel,k):
    bits=int(math.log2(k));codes={1:0x11,2:0x12,4:0x13,8:0x14,16:0x15}
    out=bytearray([codes[k]])+bytearray(pal[:k].tobytes())
    if bits:
        acc=0;nb=0
        for s in sel:
            acc=(acc<<bits)|int(s);nb+=bits
            if nb>=8:
                nb-=8;out.append((acc>>nb)&255)
    return bytes(out)

def encode_frame(rgb,prev,fi,budget):
    rows=rgb.shape[0]//8;tiles=rgb.reshape(rows,8,44,8,3).transpose(0,2,1,3,4).reshape(-1,64,3)
    pals,sels,outputs,err=candidates(tiles,prev)
    costs=np.array([0,4,15,29,49,81]);choose=np.zeros(len(tiles),np.int32)
    if fi==0:choose[:]=1
    # Lagrangian byte cost: independent tiles, then search the global lambda.
    lo,hi=0.,1e7
    for _ in range(24):
        lam=(lo+hi)/2
        c=np.argmin(err+costs[None,:]*lam,axis=1)
        if fi==0:c=np.argmin(err[:,1:]+costs[None,1:]*lam,axis=1)+1
        if int(costs[c].sum())>budget:lo=lam
        else:hi=lam;choose=c
    records={};jobs=[];rank=[]
    for t,c in enumerate(choose):
        if c:
            j=c-1;records[t]=record(pals[t,j],sels[t,j],1<<j)
            prev[t]=outputs[t,j]
            rank.append((int(err[t,c]),t))
    # A24 offload replaces only an already-dirty SH-2 record; its placeholder
    # and sidecar are aligned/padded by the packet mux, not by ad hoc DMA.
    for _,t in sorted(rank,reverse=True)[:12]:
        if len(jobs)>=5:break
        pal,sel,out,e=fit(tiles[t],24)
        old=sum((tiles[t].astype(np.int32)-prev[t].astype(np.int32)).reshape(-1)**2)
        if old-e<4096:continue
        payload=bytearray()
        for r,g,b in pal:payload+=struct.pack('>I',0x80000000|(int(b)<<16)|(int(g)<<8)|int(r))
        for q in range(0,64,4):
            a,b,c,d=map(int,sel[q:q+4]);payload+=struct.pack('>I',(b<<24)|(c<<16)|(d<<8)|a)
        jobs.append(struct.pack('>HBB',t,24,0)+payload);records[t]=b'\x73';prev[t]=out
    bodies=[];desc=[]
    for y in range(0,rows,3):
        nr=min(3,rows-y);tc=nr*44;mp=bytearray((tc+7)//8);rs=[]
        for t in range(tc):
            if y*44+t in records:mp[t>>3]|=0x80>>(t&7);rs.append(records[y*44+t])
        body=struct.pack('>HH',tc,len(rs))+mp+b''.join(rs);bodies.append(body);desc.append(struct.pack('>HBBI',y,nr,0,len(body)))
    size=26+8*len(desc)+sum(map(len,bodies))
    # FRM3: identity, PTS, keyframe flag, slice count, map-safe, count, schedule, reserved.
    fr=struct.pack('>4sIIIHBBHHH',b'FRM3',size,fi,fi,int(fi==0),len(desc),1,len(records),0,0)+b''.join(desc)+b''.join(bodies)
    side=b'DSP3'+struct.pack('>HH',len(jobs),0)+b''.join(jobs) if jobs else b''
    return fr,side

def make(source:Path,out:Path,work:Path,size:tuple[int,int],vf:str,input_args:list[str],audio:Path|None,log,end_with_picture=False,video_budget=VIDEO_BUDGET):
    work.mkdir(parents=True,exist_ok=True)
    fingerprint=hashlib.sha256(Path(__file__).read_bytes())
    for p in [source,audio]:
        if p: fingerprint.update(str((str(p.resolve()),p.stat().st_size,p.stat().st_mtime_ns)).encode())
    fingerprint.update(repr((size,vf,input_args,end_with_picture,video_budget,CACHE_VERSION)).encode())
    key=fingerprint.hexdigest()[:16];cache=work/f'{out.stem}.{key}.svm';report=cache.with_suffix('.json')
    if cache.exists():
        import shutil;shutil.copyfile(cache,out);log(f'video: {out.name}: cached RGB24 SVM');return
    w,h=size;ph=(h+7)&~7
    filt=','.join(x for x in (vf,f'fps={FPS}',f'scale={w}:{h}:flags=lanczos',f'pad=352:{ph}:0:0:black') if x)
    proc=subprocess.Popen([ffmpeg_exe(),'-v','error',*input_args,'-i',str(source),'-vf',filt,'-f','rawvideo','-pix_fmt','rgb24','pipe:1'],stdout=subprocess.PIPE)
    frames=[];prev=np.zeros((ph//8*44,64,3),np.uint8);fi=0
    try:
        while True:
            raw=proc.stdout.read(352*ph*3)
            if not raw:break
            if len(raw)!=352*ph*3:raise ValueError('short ffmpeg frame')
            frames.append(encode_frame(np.frombuffer(raw,np.uint8).reshape(ph,352,3),prev,fi,video_budget));fi+=1
            if fi%150==0:log(f'video: {out.name}: {fi} frames encoded')
        if proc.wait():raise RuntimeError('ffmpeg video failed')
    finally:
        proc.stdout.close()
        if proc.poll() is None:proc.kill();proc.wait()
    if not frames or len(frames)>2048:raise ValueError(f'unsupported movie length: {len(frames)} frames')
    duration=len(frames)/FPS;adx=work/f'{out.stem}.{key}.adx'
    cmd=[ffmpeg_exe(),'-y','-v','error']
    if audio:cmd+=['-i',str(audio)]
    else:cmd+=['-f','lavfi','-i',f'anullsrc=r={RATE}:cl=stereo']
    af=f'atrim=duration={duration},apad,atrim=duration={duration}'
    if end_with_picture:af+=f',afade=t=out:st={max(0,duration-.15)}:d=0.15'
    subprocess.run(cmd+['-af',af,'-ac','2','-ar',str(RATE),'-c:a','adpcm_adx','-f','adx',str(adx)],check=True)
    ah,ap,channels,rate,samples=_adx(adx)
    assert channels==2 and rate==RATE
    total=math.ceil(len(frames)*RATE/FPS/32);ap=(ap+bytes(total*36))[:total*36]
    a=math.sqrt(2)-math.cos(2*math.pi*500/RATE);b=math.sqrt(2)-1;c=(a-math.sqrt((a+b)*(a-b)))/b
    coef1=round(c*4096);coef2=round(-c*c*2048)
    packets=[]
    for i,(fr,side) in enumerate(frames):
        g0=i*total//len(frames);g1=(i+1)*total//len(frames);snd=ap[g0*36:g1*36]
        if i==0:snd=ah+snd
        length=32+len(side)+len(fr)+len(snd)
        if length>70000:raise ValueError('packet exceeds Saturn buffer')
        packets.append(struct.pack('>4sIIIIIII',b'PKT0',length,i,i,len(fr),len(snd),int(i==0),len(side))+side+fr+snd)
    start=(96+len(frames)*8+2047)&~2047;pos=start;index=bytearray()
    for p in packets:index+=struct.pack('>II',pos,len(p));pos+=len(p)
    header=bytearray(struct.pack('>4sHHHHHHIIIIIIII',b'SVM1',0x100,96,w,h,FPS,1,len(frames),96,start,RATE,2,18,32,len(ah)).ljust(96,b'\0'))
    struct.pack_into('>hhH',header,48,coef1,coef2,500);struct.pack_into('>I',header,56,total*32)
    cache.write_bytes(header+index+bytes(start-96-len(index))+b''.join(packets))
    report.write_text(json.dumps({'frames':len(frames),'bytes':pos,'bytes_per_second':pos/duration,'max_packet':max(map(len,packets)),'width':w,'height':h,'audio_samples':total*32},indent=2)+'\n')
    import shutil;shutil.copyfile(cache,out);log(f'video: {out.name}: {len(frames)} RGB24 frames, {pos/duration:.0f} B/s, stereo ADX {RATE} Hz')
