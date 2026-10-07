#!/usr/bin/env python3
"""Encoder/real decoder round trip; optionally decode every baked movie packet."""
from pathlib import Path
import ctypes as C
import struct
import subprocess
import sys
import tempfile
import numpy as np
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/saturn'))
import svm
with tempfile.TemporaryDirectory(prefix='svm-codec-') as tmp:
    so=Path(tmp)/'decoder.so';vendor=ROOT/'third_party/sv24'
    subprocess.run(['clang','-shared','-fPIC','-O2','-fsanitize=undefined','-fsanitize-trap=undefined','-I',str(vendor),
                    str(Path(__file__).with_name('svm_decode.c')),str(vendor/'svm.c'),str(vendor/'sv24_slice.c'),
                    str(vendor/'sv24_frame_v03.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));decode=lib.decode_packet;decode.argtypes=[C.c_void_p,C.c_uint32,C.c_void_p];decode.restype=C.c_int
    pitch=C.c_uint.in_dll(lib,'sv24_output_tiles_x');height=C.c_uint.in_dll(lib,'sv24_output_tile_rows')
    cells=np.zeros(44*30*64,np.uint32)
    def run(packet):
        return decode(C.create_string_buffer(bytes(packet)),len(packet),cells.ctypes.data)
    def mux(fr,side=b''):
        return struct.pack('>4sIIIIIII',b'PKT0',32+len(side)+len(fr),0,0,len(fr),0,0,len(side))+side+fr
    rng=np.random.default_rng(1024);prev=np.zeros((44*3,64,3),np.uint8)
    for fi in range(8):
        image=rng.integers(0,256,(24,352,3),np.uint8) if fi%3 else np.zeros((24,352,3),np.uint8)
        fr,side=svm.encode_frame(image,prev,fi,1800 if fi%2 else 16000);packet=mux(fr,side)
        assert run(packet)==0
        actual=cells[:44*3*64].reshape(44*3,64)
        expected=0x80000000|prev[:,:,0].astype(np.uint32)|(prev[:,:,1].astype(np.uint32)<<8)|(prev[:,:,2].astype(np.uint32)<<16)
        # Skipped black padding is allowed to retain initial transparent zero.
        assert np.array_equal(actual&0xffffff,expected&0xffffff),fi
        for cut in (0,1,31,len(packet)-1):assert run(packet[:cut])!=0
    # Two independent jobs may not overlap on the slave/master output surface.
    corrupt=bytearray(fr);corrupt[34:36]=corrupt[26:28]
    assert run(mux(corrupt,side))!=0
    bad=bytearray(packet);struct.pack_into('>I',bad,28,0xffffffff);assert run(bad)!=0
    total=0
    for path in map(Path,sys.argv[1:]):
        data=path.read_bytes();frames,index=struct.unpack_from('>II',data,16);cells.fill(0)
        for i in range(frames):
            at,size=struct.unpack_from('>II',data,index+i*8)
            result=run(data[at:at+size]);assert result==0,(path,i,result)
        w,h=struct.unpack_from('>HH',data,8);cols=(w+7)//8;rows=(h+7)//8
        expected=cells.reshape(30,44,64)[:rows,:cols].copy().reshape(-1)
        cells.fill(0xabcdef01);cells[:rows*cols*64]=0;pitch.value=cols;height.value=rows
        for i in range(frames):
            at,size=struct.unpack_from('>II',data,index+i*8)
            assert run(data[at:at+size])==0,(path,i,'compact')
            assert np.all(cells[rows*cols*64:]==0xabcdef01),(path,i,'surface overwrite')
        assert np.array_equal(cells[:len(expected)],expected),(path,'compact RGB mismatch')
        pitch.value=44;height.value=30
        total+=frames;print(f'PASS: {path.name}: {frames} packets including final frame')
    print(f'PASS: RGB24/temporal/A24 round trip, truncated packets, size overflow, overlapping slices; {total} asset packets (UBSan)')
