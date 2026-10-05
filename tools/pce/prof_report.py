#!/usr/bin/env python3
"""Summarise a prof_dump file (address = bank<<13 | offset in the bank) by function, using the ELF symbol table."""
import subprocess,sys
from collections import Counter
from pathlib import Path
elf,prof=sys.argv[1],sys.argv[2];top=int(sys.argv[3]) if len(sys.argv)>3 else 25
nm=Path(__file__).resolve().parents[3]/'PCE/llvm-mos8/bin/llvm-nm'
WINDOW={104:0x4000,105:0x6000,106:0x8000,107:0xa000,108:0xc000}
syms=[]
for line in subprocess.check_output([str(nm),'-n','-S',elf],text=True).splitlines():
    f=line.split()
    if len(f)==4:
        a=int(f[0],16);syms.append((a,int(f[1],16),f[3]))
    elif len(f)==3: syms.append((int(f[0],16),0,f[2]))
syms.sort()
import bisect
keys=[s[0] for s in syms]
def lookup(addr):
    bank,off=addr>>13,addr&0x1fff
    window=WINDOW.get(bank,0x6000)
    if bank>=0x70 or bank<0x68: window=0x6000 if bank>=0x69 else window
    elf_addr=0x1000000|bank<<16|(window+off)
    i=bisect.bisect_right(keys,elf_addr)-1
    return syms[i][2] if i>=0 else hex(addr)
total=Counter();all_=0
for line in open(prof):
    a,c=line.split();a=int(a,16);c=int(c)
    if a==0: continue
    total[lookup(a)]+=c;all_+=c
print('total cycles',all_)
for n,c in total.most_common(top):print(f'{c:10d} {100*c/all_:5.1f}%  {n}')
