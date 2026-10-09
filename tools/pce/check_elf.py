"""Reject missing functions, expensive numeric helpers and overflowing banks."""
import json
from pathlib import Path
import re
import subprocess
import sys
nm=Path(sys.argv[1]).resolve();elf=Path(sys.argv[2]).resolve()
output=subprocess.check_output([str(nm),'--undefined-only',str(elf)],text=True)
allowed=re.compile(r'__cd_(app_elf|s[1-7](_packed)?_bin|font_bin|ui(_packed)?_bin|victory_bin|voice[0-3]_bin)__sector$')
bad=[line for line in output.splitlines() if not allowed.fullmatch(line.split()[-1])]
if bad:raise SystemExit('Unresolved application symbols:\n'+'\n'.join(bad))
symbols=subprocess.check_output([str(nm),str(elf)],text=True)
addresses={p[2]:int(p[0],16) for line in symbols.splitlines()
           if len(p:=line.split())==3 and p[1].lower()=='t'}
all_addresses={p[2]:int(p[0],16) for line in symbols.splitlines()
               if len(p:=line.split())==3}
# These functions are entered through fixed overlay calls. Moving their
# bodies without their callers otherwise links successfully and jumps into
# a different bank's code. Resident copy/admission also has linker guards.
fixed_banks={'ui_vram':113,'panel_draw':115,'loader_card_restore':113,
             'phys_call':129,'cell_call':129,'cell':129,'spawn_clear':129,'allocate':129}
if 'space_vdc1_actor_pass' in addresses:
    fixed_banks.update(space_vdc1_actor_pass=119,space_vdc1_sprite_body=116,
                       video_arena_bg_clear_body=113,arena_bg_palette=113)
if 'pce_sgx_sky_stream_body' in addresses:
    fixed_banks.update(zx02_arcade=130,pause_show_body=130,pause_hide_body=130,ui_animation_body=113,
        pce_sgx_sky_load_body=130,pce_sgx_sky_stream_body=130,
        pce_sgx_herd_sky_load_body=128,pce_sgx_herd_front_draw_body=128,pce_sgx_hull_retire_body=130,
        pce_sgx_story_world_body=128,
        pce_sgx_platform_actor_pass_body=114,pce_sgx_projectile_body=119,
        pce_sgx_vdc1_sat_upload_body=120,pce_sgx_sprite_upload_body=128,
        pce_sgx_cache_upload_body=128,hull_body=128,platform_begin_body=128,
        pce_sgx_select_body=110,pce_sgx_budget_body=110,pce_sgx_ui_load_body=124,pce_sgx_ui_end_body=124,
        hull_mirror_body=120,hull_claim_tail_body=128,ui_fade_body=128)
    animation=addresses.get('pce_sgx_select_animate_body',0)
    if animation and ((animation>>16)&255!=135 or not 0xc000<=animation&65535<0xe000):
        raise SystemExit('Selection animation must execute through MPR6 in bank 135')
    words=all_addresses.get('sprite_words1',0)
    if words>>16!=0x168 or not 0x4000<=words&65535<0x6000:
        raise SystemExit('VDC1 sprite addresses must remain in always-mapped bank $68')
    owners=all_addresses.get('pattern_owner1',0)&65535
    if not 0x2000<=owners<0x3bf0:
        raise SystemExit('VDC1 pattern owners must remain in console RAM')
    admission=addresses.get('video_sprite_optional',0)
    if (admission>>16)&255!=107 or not 0xa000<=admission&65535<0xc000:
        raise SystemExit('SGX optional sprite admission must remain in the always-mapped work bank')
for name,bank in fixed_banks.items():
    address=addresses.get(name,0)
    if (address>>16)&255!=bank or not 0x6000<=address&65535<0x8000:
        raise SystemExit(f'{name} must stay in overlay bank ${bank:02x}')
if re.search(r'\b__(?:u?div|u?mod|mul|add|sub|eq|ne|lt|le|gt|ge)(?:sf|df|di)\w*\b',symbols):
    raise SystemExit('Floating-point or 64-bit arithmetic reached the target image')
sections=subprocess.check_output([str(nm.parent/'llvm-readelf'),'-S',str(elf)],text=True)
rows=[];banks={}
for name,kind,address,size in re.findall(r'\[\s*\d+\]\s+(\S+)\s+(PROGBITS|NOBITS)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)',sections):
    address,size=int(address,16),int(size,16)
    if address>>16 in range(0x168,0x188) or address>>16 in (0x190,0x191,0x192):
        bank=(address>>16)&255;window={104:0x4000,105:0x6000,106:0x8000,107:0xa000,108:0xc000,109:0x6000,110:0x6000,111:0x6000,112:0x6000,113:0x6000,114:0x6000,115:0x6000,116:0x6000,117:0x6000,118:0x6000,119:0x6000,120:0x6000,121:0x6000,122:0x6000,123:0x6000,124:0x6000,128:0x6000,129:0x6000,130:0x6000,131:0x6000,144:0x6000,145:0x6000,146:0x6000}.get(bank,0xc000)
        used=(address&65535)+size-window
        if not 0<=used<=8192:raise SystemExit(f'{name} exceeds bank ${bank:02x}')
        banks[bank]=max(banks.get(bank,0),used)
        rows.append(dict(name=name,bank=bank,address=address&65535,bytes=size))
    elif address>>16==0xf8 and name in ('.bss','.noinit'):
        if (address&65535)+size>0x3bf0:raise SystemExit('Console state overlaps the native-test trampoline at $3bf0'
                              ' or the reserved software stack')
        rows.append(dict(name=name,address=address&65535,bytes=size))
# HuC6280 direct page lives at $2000. LLVM-MOS can promote arrays but
# leave absolute indexed loads/stores at $00xx, which addresses hardware.
# Inspect executable sections so future promotions fail the build.
disassembly=subprocess.check_output([str(nm.parent/'llvm-objdump'),'-d',str(elf)],text=True)
invalid=[]
for line in disassembly.splitlines():
    match=re.search(r':\s+([0-9a-f]{2}) ([0-9a-f]{2}) 00\s+\w+\s+\$[0-9a-f]+,[xy]\b',line)
    if match:invalid.append(line.strip())
if invalid:raise SystemExit('Absolute indexed access to HuC6280 hardware from promoted RAM:\n'+'\n'.join(invalid))
compiler=subprocess.check_output([str(nm.parent/'mos-pce-cd-clang'),'--version'],text=True).strip()
flags=(elf.parent/'build-flags').read_text().split()
report=dict(compiler=compiler,flags=flags,sections=rows,
            banks=[dict(bank=b,used=n,free=8192-n) for b,n in sorted(banks.items())],
            arcade_asset_limit=0x1e0000,arcade_tile_directory_bytes=0x20000,
            vram_platform_cache=[dict(name=n,start_word=a,end_word=b) for n,a,b in [('HUD generations',0x4800,0x5000),('hero generations',0x5000,0x5600),('actors/foreground',0x5600,0x7800),('counter generations',0x7800,0x7b00),('actors/foreground spare',0x7b00,0x7e00)]],
            vram_normal=[dict(name=n,start_word=a,end_word=b) for n,a,b in [('BAT',0,0x800),('BG cache',0x800,0x4000),('dialog characters',0x4000,0x4200),('font',0x4200,0x4800),('sprite cache',0x4800,0x7800),('HUD/clip patterns',0x7800,0x7e00),('SAT alternate',0x7e00,0x7f00),('SAT',0x7f00,0x8000)]],
            vram_floor=[dict(name=n,start_word=a,end_word=b) for n,a,b in [('BAT',0,0x2000),('road characters',0x2000,0x3000),('sky',0x3000,0x4200),('font',0x4200,0x4800),('sprite cache',0x4800,0x7800),('HUD/clip patterns',0x7800,0x7e00),('SAT alternate',0x7e00,0x7f00),('SAT',0x7f00,0x8000)]])
(elf.parent/'runtime.json').write_text(json.dumps(report,indent=2)+'\n')
