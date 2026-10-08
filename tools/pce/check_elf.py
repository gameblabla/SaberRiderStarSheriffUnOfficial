"""Reject missing functions, expensive numeric helpers and overflowing banks."""
import json
from pathlib import Path
import re
import subprocess
import sys
nm=Path(sys.argv[1]).resolve();elf=Path(sys.argv[2]).resolve()
output=subprocess.check_output([str(nm),'--undefined-only',str(elf)],text=True)
allowed=re.compile(r'__cd_(app_elf|s[1-7]_bin|font_bin|ui_bin|victory_bin|voice[0-3]_bin)__sector$')
bad=[line for line in output.splitlines() if not allowed.fullmatch(line.split()[-1])]
if bad:raise SystemExit('Unresolved application symbols:\n'+'\n'.join(bad))
symbols=subprocess.check_output([str(nm),str(elf)],text=True)
if re.search(r'\b__(?:u?div|u?mod|mul|add|sub|eq|ne|lt|le|gt|ge)(?:sf|df|di)\w*\b',symbols):
    raise SystemExit('Floating-point or 64-bit arithmetic reached the target image')
sections=subprocess.check_output([str(nm.parent/'llvm-readelf'),'-S',str(elf)],text=True)
rows=[];banks={}
for name,kind,address,size in re.findall(r'\[\s*\d+\]\s+(\S+)\s+(PROGBITS|NOBITS)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)',sections):
    address,size=int(address,16),int(size,16)
    if address>>16 in range(0x168,0x188):
        bank=(address>>16)&255;window={104:0x4000,105:0x6000,106:0x8000,107:0xa000,108:0xc000,109:0x6000,110:0x6000,111:0x6000,112:0x6000,113:0x6000,114:0x6000,115:0x6000,116:0x6000,117:0x6000,118:0x6000,119:0x6000,120:0x6000,121:0x6000,122:0x6000,123:0x6000,124:0x6000,128:0x6000,129:0x6000,130:0x6000,131:0x6000}.get(bank,0xc000)
        used=(address&65535)+size-window
        if not 0<=used<=8192:raise SystemExit(f'{name} exceeds bank ${bank:02x}')
        banks[bank]=max(banks.get(bank,0),used)
        rows.append(dict(name=name,bank=bank,address=address&65535,bytes=size))
    elif address>>16==0xf8 and name in ('.bss','.noinit'):
        if (address&65535)+size>0x3bf0:raise SystemExit('Console state overlaps the native-test trampoline at $3bf0'
                              ' or the reserved software stack')
        rows.append(dict(name=name,address=address&65535,bytes=size))
compiler=subprocess.check_output([str(nm.parent/'mos-pce-cd-clang'),'--version'],text=True).strip()
report=dict(compiler=compiler,flags=['-std=gnu11','-Os','-flto','-mlto-zp=0'],sections=rows,
            banks=[dict(bank=b,used=n,free=8192-n) for b,n in sorted(banks.items())],
            arcade_asset_limit=0x1e0000,arcade_tile_directory_bytes=0x20000,
            vram_normal=[dict(name=n,start_word=a,end_word=b) for n,a,b in [('BAT',0,0x800),('BG cache',0x800,0x4000),('dialog characters',0x4000,0x4200),('font',0x4200,0x4800),('sprite cache',0x4800,0x7800),('HUD/clip patterns',0x7800,0x7e00),('SAT alternate',0x7e00,0x7f00),('SAT',0x7f00,0x8000)]],
            vram_floor=[dict(name=n,start_word=a,end_word=b) for n,a,b in [('BAT',0,0x2000),('road characters',0x2000,0x3000),('sky',0x3000,0x4200),('font',0x4200,0x4800),('sprite cache',0x4800,0x7800),('HUD/clip patterns',0x7800,0x7e00),('SAT alternate',0x7e00,0x7f00),('SAT',0x7f00,0x8000)]])
(elf.parent/'runtime.json').write_text(json.dumps(report,indent=2)+'\n')
