#!/usr/bin/env python3
"""Replay arena gameplay data from the reported SGX saves on a rebuilt disc."""
import argparse
import gzip
import json
import struct
import tempfile
from pathlib import Path
from emulator import Emulator, boot, symbol
from test_sgx_rendering import Rendering
from test_sgx_fast_math import Code, STUB, MARKER

BLANK = 0x3ff


def sections(path):
    data = gzip.decompress(path.read_bytes())
    width, height = struct.unpack_from('<II', data, 24)
    pos = 32 + width * height * 3
    result = {}
    while pos < len(data):
        name = data[pos:pos+32].split(b'\0')[0].decode()
        size, = struct.unpack_from('<I', data, pos+32)
        cursor, end = pos+36, pos+36+size
        fields = {}
        while cursor < end:
            length = data[cursor]
            key = data[cursor+1:cursor+1+length].decode()
            size, = struct.unpack_from('<I', data, cursor+1+length)
            cursor += 5+length
            fields[key] = data[cursor:cursor+size]
            cursor += size
        result[name] = fields
        pos = end
    return result


def read(e, space, address, size):
    return bytes.fromhex(e.call('asread', space, address, size)['hex'])


def stride_check(out, r):
    payload = bytes((i*37+13)&255 for i in range(2304))
    with tempfile.TemporaryDirectory(prefix='arena-stride-', dir=out) as tmp, Emulator(out/'saber_rider.cue', tmp, sgx=True) as e:
        boot(e, r.address)
        e.write(0x1d0000, payload, space='acram')
        for stride in (0, 0x800, 0x1000, 0x1800):
            e.write(0, bytes(4096), space='vram0')
            e.write(0x4800*2, bytes(2304), space='vram0')
            c = Code()
            for address,value in ((0,5),(2,0xcc),(3,stride>>8),
                                  (0x20f3,0xcc),(0x20f4,stride>>8),
                                  (0x2002,0),(0x2003,29),(0x2004,0),
                                  (0x2005,0),(0x2006,0x48),
                                  (0x2007,0),(0x2008,9)):
                c.lda_imm(value);c.sta(address)
            c.lda_imm(0);c.ldx_imm(0)
            c.jsr(symbol(out/'app.elf','arcade_vram_to'))
            e.write(MARKER, bytes(1));e.write(STUB,c.finish())
            for register,value in (('P',4),('SP',253),('MPR2',104),('MPR3',108),('MPR6',109),('PC',STUB)):
                e.call('register_set',register,value)
            e.run(2)
            assert e.memory(MARKER,1) == b'\x01', ('upload did not return',stride)
            assert read(e,'vram0',0x4800*2,2304) == payload, ('noncontiguous upload',stride)
            assert read(e,'vram0',0,4096) == bytes(4096), ('upload wrapped into BAT',stride)
    return 4


def verify(out, saves):
    out = out.resolve()
    r = Rendering(out)
    arena_offset = symbol(out/'app.elf', 'trigger_cache') & 8191
    report = {}
    for path in sorted(saves.glob('*.mc0')):
        state = sections(path)
        # Standard Mednafen stores the 24 Super CD banks starting at $68.
        # The project headless build starts its extended RAM at $50 instead.
        ram = state['HuC']['SysCardRAM']
        base = 0x68 if len(ram) == 24*8192 else 0x50
        arena = ram[(108-base)*8192+arena_offset:][:570]
        assert len(arena) == 570
        old_vram = state['VDC']['VRAM']
        old_cells = struct.unpack('<2048H', old_vram[:4096])
        bad = [(i%64, i//64, cell) for i,cell in enumerate(old_cells)
               if cell and not (cell>>12 == 15 and 128 <= (cell&4095) < 640)]
        assert bad, 'fixture must contain the reported BAT corruption'
        name = 'sprite' if 'sprite' in path.name else 'terrain'
        with tempfile.TemporaryDirectory(prefix='arena-save-', dir=out) as tmp, Emulator(out/'saber_rider.cue', tmp, sgx=True) as e:
            boot(e, r.address)
            r.dialogs(e)
            r.stage(e, 6)
            # Replay gameplay values, keeping fresh code, cache ownership,
            # compositor state and display registers from the new disc.
            e.write(108*8192+arena_offset, arena, space='physical')
            r.field(e, 'wave', 1 if name == 'sprite' else 2)
            samples = 0
            for frame in range(180):
                e.run(1)
                cells = struct.unpack('<2048H', read(e, 'vram0', 0, 4096))
                assert all(c == BLANK or (c>>12 == 15 and 128 <= (c&4095) < 640) for c in cells), (name,frame,'rogue BAT cell')
                assert read(e, 'vram0', BLANK*32, 32) == bytes(32), (name,frame,'blank pattern overwritten')
                samples += 1
            e.screenshot(out/f'arena-{name}-after.png')
            # Pause must preserve the backdrop and blank pattern too.
            r.press(e, 8)
            e.run(30)
            assert read(e, 'vram0', BLANK*32, 32) == bytes(32)
            e.screenshot(out/f'arena-{name}-pause-after.png')
            report[name] = {'saved_bad_cells': bad, 'frames_checked': samples,
                            'pause_blank_intact': True}
    assert len(report) == 2
    report['contiguous_upload_stride_modes'] = stride_check(out,r)
    (out/'sgx-arena-saves-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/debug-sgx'))
    parser.add_argument('--saves', type=Path, default=Path('save_sgx'))
    args = parser.parse_args()
    verify(args.out, args.saves)
