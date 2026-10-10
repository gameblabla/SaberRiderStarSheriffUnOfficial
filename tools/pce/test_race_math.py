#!/usr/bin/env python3
"""Exhaustively execute the signed-byte road multiply on the native CPU."""
import argparse,json,struct,tempfile
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_campaign import Campaign

def verify(out,sgx=False):
    out=out.resolve();c=Campaign(out,sgx=sgx);elf=out/'app.elf'
    with tempfile.TemporaryDirectory(prefix='race-math-',dir=out) as base,Emulator(out/'saber_rider.cue',base,sgx=sgx) as e:
        boot(e,c.address);c.seed(e,'stage',1,1);c.field(e,'state',2);c.advance(e,2);c.dialogs(e)
        if sgx:
            # race_reset precomputes one exact 2^24 / segment-length^2
            # reciprocal for each of the track's 256 wraparound segments.
            manifest=json.loads((out/'manifest.json').read_text())
            scene=manifest['scenes'][1]
            record=scene['records']['track'];blob=(out/'s2.bin').read_bytes()
            points=[struct.unpack_from('<HHB',blob,record['offset']+i*5)
                    for i in range(256)]
            want=[]
            for i,(x,y,_heading) in enumerate(points):
                nx,ny,_=points[(i+1)&255]
                dx=((nx-x+32768)&0xffff)-32768
                dy=((ny-y+32768)&0xffff)-32768
                if dx>4096:dx-=8192
                elif dx<-4096:dx+=8192
                if dy>4096:dy-=8192
                elif dy<-4096:dy+=8192
                denominator=dx*dx+dy*dy
                assert denominator>0,('zero-length track segment',i)
                want.append((1<<24)//denominator)
            columns=symbol(elf,'columns')
            got=struct.unpack('<256H',e.memory(columns+1152,512))
            assert list(got)==want,('SGX road reciprocal table',next(
                (i,got[i],want[i]) for i in range(256) if got[i]!=want[i]))
        # Run a standalone loop in a retired renderer bank with IRQs masked.
        # The work bank containing quarter squares stays mapped. Real CPU
        # stores configure the Arcade port; debugger I/O pokes bypass handlers.
        code=bytearray();labels={};branches=[]
        def emit(*b):code.extend(b)
        def store(address,value):emit(0xa9,value,0x8d,address&255,address>>8)
        def branch(op,label):emit(op,0);branches.append((len(code)-1,label))
        for address,value in ((0x1a32,0),(0x1a33,0),(0x1a34,29),(0x1a35,0),(0x1a36,0),(0x1a37,1),(0x1a38,0),(0x1a39,0x11),(0x3b90,0),(0x3b91,0)):
            store(address,value)
        labels['loop']=len(code)
        emit(0xad,0x90,0x3b,0xae,0x91,0x3b,0xa0,0x47)
        fn=symbol(elf,'smul8');emit(0x20,fn&255,fn>>8,0x8d,0x30,0x1a,0x8a,0x8d,0x30,0x1a)
        emit(0xc0,0x47);branch(0xd0,'failed')
        emit(0xee,0x90,0x3b);branch(0xd0,'loop')
        emit(0xee,0x91,0x3b);branch(0xd0,'loop')
        store(0x3b92,1);branch(0x80,'done')
        labels['failed']=len(code);store(0x3b92,2)
        labels['done']=len(code);emit(0x4c,(0xc000+len(code))&255,(0xc000+len(code))>>8)
        for offset,label in branches:
            delta=labels[label]-offset-1;assert -128<=delta<=127;code[offset]=delta&255
        e.call('register_set','MPR6',124);e.write(0xc000,code);e.write(0x3b92,b'\0')
        for key,val in [('P',4),('SP',253),('MPR3',111),('MPR6',124),('PC',0xc000)]:e.call('register_set',key,val)
        e.run(600)
        assert e.memory(0x3b92,1)==b'\1','Multiply loop failed or did not finish; Y must be preserved'
        actual=b''.join(bytes.fromhex(e.call('asread','acram',0x1d0000+i,16384)['hex']) for i in range(0,131072,16384))
        expected=b''.join(struct.pack('<h',(a if a<128 else a-256)*(b if b<128 else b-256)) for b in range(256) for a in range(256))
        assert actual==expected,'Signed road multiply differs from exact products'
    print('Native road multiply: all 65,536 signed-byte pairs passed')
    if sgx:print('SGX road reciprocals: all 256 track segments exact')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/pce'));p.add_argument('--sgx',action='store_true');a=p.parse_args();verify(a.out,a.sgx)
